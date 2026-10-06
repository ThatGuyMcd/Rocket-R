#!/usr/bin/env python3
"""Create, build, check and distribute portable Rocket mods."""
from __future__ import annotations
import argparse
import binascii
import hashlib
import json
import math
import platform
from pathlib import Path
import re
import struct
import subprocess
import sys
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
import zipfile
import io
import wave

ROOT = Path(__file__).resolve().parents[1]
MODULES = {name: 1 for name in ('core', 'lifecycle', 'resources', 'saves',
    'native_objects', 'diagnostics', 'input', 'events', 'systems', 'hud', 'native_audio',
    'meshes', 'actors', 'custom_scenes', 'custom_collision', 'custom_audio', 'native_render', 'commands', 'asset_layers')}
ROM_SIZE = 0xC00000
ASSET_START = 0xB0460


def require(ok, message):
    if not ok:
        raise ValueError(message)


def safe_name(name):
    return (isinstance(name, str) and bool(name) and len(name) <= 240 and not name.startswith('/')
            and '\\' not in name and ':' not in name
            and all(p not in ('', '.', '..') for p in name.split('/'))
            and all(32 <= ord(c) != 127 for c in name))


def valid_id(value):
    return isinstance(value, str) and re.fullmatch(r'[a-z0-9_-]{1,80}', value) is not None


def validate_metadata(metadata, manifest):
    require(type(metadata.get('api', 1)) is int and metadata.get('api', 1) in (1, 2), 'Unsupported Rocket API')
    if metadata.get('api', 1) == 1:
        return
    require(metadata.get('schema') == 1, 'Unsupported Rocket metadata schema')
    limits = {'requires': 64, 'resources': 4096, 'input_actions': 64, 'settings_ui': 128}
    for name, limit in limits.items():
        require(isinstance(metadata.get(name, {}), dict) and len(metadata.get(name, {})) <= limit, f'Invalid {name} list')
    for name, version in metadata.get('requires', {}).items():
        require(type(version) is int and 0 < version <= MODULES.get(name, 0), f'Unavailable SDK module: {name}')
    for name, entry in metadata.get('resources', {}).items():
        require(safe_name(name) and isinstance(entry, dict) and safe_name(entry.get('file')), 'Unsafe resource')
        require(isinstance(entry.get('type', 'binary'), str) and len(entry.get('type', 'binary')) <= 64, 'Invalid resource type')
    for name, action in metadata.get('input_actions', {}).items():
        require(valid_id(name) and isinstance(action, dict), 'Invalid input action')
        require(isinstance(action.get('name', name), str) and len(action.get('name', name)) <= 160, 'Invalid action label')
        keyboard, controller, n64 = action.get('keyboard', -1), action.get('controller', -1), action.get('n64', 0)
        require(type(keyboard) is int and (keyboard in range(-1, 512) or keyboard in range(2001, 2006) or keyboard in range(2100, 2104) or keyboard in range(2200, 2204)), 'Invalid keyboard/mouse input')
        require(type(controller) is int and (controller in range(-1, 21) or controller in range(1000, 1012)), 'Invalid controller input')
        require(type(n64) is int and 0 <= n64 <= 65535, 'Invalid N64 touch mask')
    for key, widget in metadata.get('settings_ui', {}).items():
        require(valid_id(key) and widget == 'toggle', 'Unknown settings widget')
        require(any(o.get('id') == key and o.get('type') == 'Enum' and len(o.get('options', [])) == 2
                    for o in manifest.get('config_schema', {}).get('options', [])), 'Toggle widgets need a two-choice Enum')
    systems = metadata.get('systems', [])
    require(isinstance(systems, list) and len(systems) <= 64 and all(safe_name(s) and len(s) < 96 for s in systems), 'Invalid systems list')
    commands = metadata.get('commands', [])
    require(isinstance(commands, list) and len(commands) <= 32, 'Invalid commands list')
    seen = set()
    for command in commands:
        require(isinstance(command, dict) and valid_id(command.get('id')) and command['id'] not in seen
                and isinstance(command.get('name'), str) and len(command['name']) <= 160, 'Invalid or duplicate command')
        seen.add(command['id'])


def bps_number(value):
    require(value >= 0, 'Negative BPS integer')
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0:
            result.append(byte | 128)
            return result
        result.append(byte)
        value -= 1


def make_asset_patch(source, target):
    require(len(source) == ROM_SIZE and len(target) == ROM_SIZE, 'Use a canonical 12 MiB US cartridge')
    require(source[:4] == bytes.fromhex('80371240'), 'The source must be a big-endian .z64 cartridge')
    require(source[:ASSET_START] == target[:ASSET_START], 'Asset patches cannot change boot/game code or the header')
    output = bytearray(b'BPS1') + bps_number(len(source)) + bps_number(len(target)) + bps_number(0)
    # Deliberately simple SourceRead/TargetRead encoding. No source ROM bytes
    # are copied into a patch, apart from the authored replacement literals.
    position = 0
    while position < len(source):
        equal = source[position] == target[position]
        end = position + 1
        while end < len(source) and (source[end] == target[end]) == equal:
            end += 1
        output += bps_number(((end-position-1) << 2) | (0 if equal else 1))
        if not equal:
            output += target[position:end]
        position = end
    output += struct.pack('<II', binascii.crc32(source), binascii.crc32(target))
    output += struct.pack('<I', binascii.crc32(output))
    return bytes(output)


def validate_asset_patch(data):
    require(len(data) >= 16 and data[:4] == b'BPS1', 'Invalid BPS patch')
    require(binascii.crc32(data[:-4]) == struct.unpack_from('<I', data, len(data)-4)[0], 'BPS checksum failed')
    cursor, end = 4, len(data)-12
    def number():
        nonlocal cursor
        value, shift = 0, 1
        for _ in range(10):
            require(cursor < end, 'Truncated BPS integer')
            byte = data[cursor]
            cursor += 1
            value += (byte & 127) * shift
            require(value <= 0xFFFFFFFF, 'BPS integer exceeds the cartridge format')
            if byte & 128:
                return value
            shift <<= 7
            value += shift
        raise ValueError('Invalid BPS integer')
    source_size, target_size, metadata = number(), number(), number()
    require(source_size == target_size == ROM_SIZE, 'BPS must preserve the US cartridge size')
    require(metadata <= end-cursor, 'Truncated BPS metadata')
    cursor += metadata
    target, source_copy, target_copy = 0, 0, 0
    while cursor < end:
        action = number()
        mode, length = action & 3, (action >> 2) + 1
        require(length <= target_size-target, 'BPS writes outside the cartridge')
        if mode == 1:
            require(target >= ASSET_START, 'BPS changes game code/header')
            require(length <= end-cursor, 'Truncated BPS literal')
            cursor += length
        elif mode >= 2:
            offset = number()
            delta = -(offset >> 1) if offset & 1 else offset >> 1
            if mode == 2:
                source_copy += delta
                require(0 <= source_copy <= source_size-length, 'Invalid BPS source copy')
                require(target >= ASSET_START or source_copy == target, 'BPS copies different game code')
                source_copy += length
            else:
                target_copy += delta
                require(target >= ASSET_START and 0 <= target_copy < target, 'Invalid BPS target copy')
                target_copy += length
        target += length
    require(cursor == end and target == target_size, 'BPS target is incomplete')


def validate_package(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) <= 16384 and len({n.lower() for n in names}) == len(names), 'Duplicate/excessive package entries')
        require(sum(i.file_size for i in archive.infolist()) <= 512*1024*1024, 'Package expands past 512 MiB')
        for name in names:
            require(safe_name(name.rstrip('/')), f'Unsafe filename: {name}')
            require(Path(name).suffix.lower() not in ('.z64','.n64','.v64','.rom','.exe','.so','.dll','.dylib'), f'Unwanted payload: {name}')
            require(archive.getinfo(name).file_size <= 256*1024*1024, f'Oversized entry: {name}')
        manifest = json.loads(archive.read('mod.json'))
        require(re.fullmatch(r'[a-z0-9_-]+',manifest['id']) is not None, 'Invalid mod ID')
        require(manifest['game_id'] == 'rocket', 'Wrong game ID')
        require(('mod_syms.bin' in names) == ('mod_binary.bin' in names), 'Missing code/symbol pair')
        metadata = json.loads(archive.read('rocket.json')) if 'rocket.json' in names else {}
        validate_metadata(metadata, manifest)
        api = metadata.get('api', 1)
        require(api in (1,2), 'Unsupported Rocket API')
        if api == 2:
            require(metadata.get('schema') == 1, 'Unsupported Rocket metadata schema')
            for name, version in metadata.get('requires',{}).items():
                require(type(version) is int and 0 < version <= MODULES.get(name,0), f'Unavailable SDK module: {name}')
            for name, entry in metadata.get('resources',{}).items():
                require(safe_name(name) and safe_name(entry['file']), 'Unsafe resource')
                require(entry['file'] in names and archive.getinfo(entry['file']).file_size <= 16*1024*1024, f'Missing/oversized resource: {name}')
            mode = metadata.get('activation','restart')
            require(mode in ('restart','managed'), 'Invalid activation mode')
            if mode == 'managed':
                require(not metadata.get('exclusive_resources'), 'Managed mods cannot replace exclusive native systems')
                require('patch.bps' not in names, 'ROM patches need restart activation')
                if 'mod_syms.bin' in names:
                    symbols = archive.read('mod_syms.bin')
                    require(len(symbols) >= 52 and symbols[:8] == b'N64RSYMS' and struct.unpack_from('<I',symbols,8)[0] == 1, 'Unsupported managed symbol format')
                    require(struct.unpack_from('<I',symbols,28)[0] == struct.unpack_from('<I',symbols,44)[0] == 0, 'Raw replacements/hooks need restart activation')
            if 'patch.bps' in names:
                validate_asset_patch(archive.read('patch.bps'))
        return {'id':manifest['id'],'version':manifest['version'],'api':api,
                'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'files':len(names)}


def initialise(path, mod_id):
    require(re.fullmatch(r'[a-z0-9_-]{1,80}',mod_id) is not None, 'Choose a lowercase mod ID')
    require(not path.exists() or not any(path.iterdir()), 'The project folder must be empty')
    path.mkdir(parents=True,exist_ok=True)
    (path/'src').mkdir()
    (path/'mod.toml').write_text(f'''[manifest]
id = "{mod_id}"
version = "0.1.0"
display_name = "{mod_id.replace('_',' ').title()}"
authors = ["Your name"]
game_id = "rocket"
minimum_recomp_version = "1.1.0"
dependencies = []

[build]
sources = ["src/**/*.c"]
''',encoding='utf-8')
    (path/'rocket.json').write_text(json.dumps({'schema':1,'api':2,'activation':'managed',
        'live_settings':True,'requires':{'lifecycle':1},'resources':{},'input_actions':{}},indent=2)+'\n',encoding='utf-8')
    (path/'src/main.c').write_text('''#include "rocket/sdk.h"

static void update(const RocketTick* tick) {
    if (tick->state != ROCKET_PLAYING) return;
    /* Add your gameplay here. Handles expire at the next authored frame. */
}
static const RocketCallbacks callbacks = {
    ROCKET_SDK_VERSION, sizeof(RocketCallbacks), update, 0, 0, 0, 0, 0
};
ROCKET_CALLBACK(rocket_on_game_ready)
void mod_ready(void) {
    if (rocket_sdk_register(&callbacks) != ROCKET_OK)
        rocket_sdk_log("Could not register this mod.");
}
''',encoding='utf-8')


def read_texture(path):
    require(path.stat().st_size <= 1024*1024, 'Texture source exceeds 1 MiB')
    if path.suffix.lower() == '.ppm':
        tokens = re.sub(r'#[^\n]*','',path.read_text(encoding='ascii')).split()
        require(len(tokens)>=4 and tokens[0]=='P3', 'PPM needs the text P3 format')
        width,height,maximum=map(int,tokens[1:4])
        require(maximum==255 and len(tokens)==4+width*height*3, 'PPM needs complete RGB data with a maximum of 255')
        rgb=list(map(int,tokens[4:]));require(all(0<=c<=255 for c in rgb),'Invalid PPM colour')
        pixels=[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)]
    else:
        try:
            from PIL import Image
        except ImportError as error:
            raise ValueError('PNG textures need Pillow: python -m pip install Pillow. P3 PPM needs no extra dependency.') from error
        with Image.open(path) as image:
            width,height=image.size
            require(width in (8,16,32) and height in (8,16,32), 'Texture dimensions must be 8, 16 or 32')
            pixels=list(image.convert('RGBA').getdata())
    require(width in (8,16,32) and height in (8,16,32), 'Texture dimensions must be 8, 16 or 32')
    require(all(a==255 for r,g,b,a in pixels), 'The current mesh material requires opaque textures')
    output=bytearray()
    for r,g,b,a in pixels: output+=struct.pack('>H',((r>>3)<<11)|((g>>3)<<6)|((b>>3)<<1)|1)
    return width,height,bytes(output)


def compile_tones(path):
    require(path.stat().st_size<=64*1024,'Tone score exceeds 64 KiB')
    score=json.loads(path.read_text(encoding='utf-8'));rate=score.get('sample_rate',22500);notes=score['notes'];volume=score.get('volume',.15)
    require(type(rate) is int and 8000<=rate<=48000 and 0<=volume<=.5 and len(notes)<=256,'Invalid tone score')
    pcm=bytearray()
    for frequency,duration in notes:
        require(math.isfinite(frequency) and math.isfinite(duration) and 0<=frequency<rate/2 and .01<=duration<=10,'Invalid note frequency/duration')
        length=round(duration*rate);require(len(pcm)+length*2<=16*1024*1024,'Tone score exceeds 16 MiB')
        for i in range(length):
            envelope=min(1,i/(rate*.005),(length-1-i)/(rate*.03))
            pcm+=struct.pack('<h',round(math.sin(2*math.pi*frequency*i/rate)*volume*envelope*32767))
    require(pcm,'Tone score is empty');output=io.BytesIO()
    with wave.open(output,'wb') as wav:
        wav.setparams((1,2,rate,0,'NONE','not compressed'));wav.writeframes(pcm)
    return output.getvalue()


def compile_mesh(path, colour='FFFFFFFF', y_up=False, texture=None):
    require(re.fullmatch(r'[0-9a-fA-F]{8}',colour) is not None, 'Mesh colour needs eight hex digits: RRGGBBAA')
    require(path.stat().st_size <= 1024*1024, 'OBJ source exceeds 1 MiB')
    positions, vertices, uvs, triangles, references = [], [], [], [], {}
    for line in path.read_text(encoding='utf-8').splitlines():
        tokens = line.split('#',1)[0].split()
        if not tokens: continue
        if tokens[0] == 'v':
            require(len(tokens) >= 4, 'OBJ vertex needs three coordinates')
            x,y,z = map(float,tokens[1:4])
            if y_up: x,y,z = x,-z,y
            require(all(math.isfinite(v) and abs(v) <= 32760 for v in (x,y,z)), 'OBJ vertex is outside the N64 coordinate range')
            positions.append((x,y,z))
        elif tokens[0] == 'vt':
            require(len(tokens)>=3,'OBJ UV needs two coordinates')
            u,v=map(float,tokens[1:3]);require(math.isfinite(u) and math.isfinite(v) and abs(u)<=16 and abs(v)<=16,'Invalid OBJ UV')
            require(abs(1-v)<=16,'Converted OBJ UV is outside the supported range');uvs.append((u,1-v))
        elif tokens[0] == 'f':
            face = []
            for token in tokens[1:]:
                parts=token.split('/');index = int(parts[0])
                require(index != 0, 'OBJ vertex indices start at 1')
                index = index-1 if index > 0 else len(positions)+index
                require(0 <= index < len(positions), 'OBJ face references a missing vertex')
                if texture:
                    require(len(parts)>1 and parts[1], 'Textured OBJ faces need UV indices')
                    uv=int(parts[1]);require(uv!=0,'OBJ UV indices start at 1');uv=uv-1 if uv>0 else len(uvs)+uv
                    require(0<=uv<len(uvs),'OBJ face references a missing UV')
                    key=(index,uv)
                    if key not in references:
                        references[key]=len(vertices);vertices.append((*positions[index],*uvs[uv]))
                    index=references[key]
                face.append(index)
            require(len(face) >= 3, 'OBJ face needs at least three vertices')
            triangles.extend((face[0],face[i],face[i+1]) for i in range(1,len(face)-1))
        require(len(positions)<=4096 and len(vertices) <= 4096 and len(triangles) <= 4096, 'Mesh exceeds 4096 vertices/triangles')
    if not texture: vertices=positions
    require(vertices and triangles, 'OBJ has no triangle geometry')
    output = bytearray(b'RRM2') + struct.pack('>III',2 if texture else 1,len(vertices),len(triangles))
    if texture: width,height,pixels=read_texture(texture);output+=struct.pack('>II',width,height)
    for vertex in vertices:
        output += struct.pack('>fffI',*vertex[:3],int(colour,16))
        if texture: output+=struct.pack('>ff',*vertex[3:])
    for triangle in triangles: output += struct.pack('>III',*triangle)
    if texture: output+=pixels
    return bytes(output)


def symbol_search(pattern, kind='all', limit=50):
    require(0 < limit <= 500, 'Symbol limit must be 1..500')
    protected_file = ROOT/'symbols/protected-functions.json'
    generated = ROOT/'generated/mod_protection.generated.hpp'
    protected = set(json.loads(protected_file.read_text()) if protected_file.exists() else
                    re.findall(r'// (\w+)', generated.read_text()) if generated.exists() else [])
    result = []
    for category, filename, group in (('function', 'rocket.functions.dump.toml', 'functions'),
                                       ('data', 'rocket.data.dump.toml', 'symbols')):
        if kind not in ('all', category): continue
        source = ROOT/'symbols'/filename
        if not source.exists(): source = ROOT/'build/generated'/filename
        for section in tomllib.loads(source.read_text(encoding='utf-8')).get('section', []):
            for entry in section.get(group, []):
                if pattern.lower() in entry['name'].lower():
                    result.append({'kind': category, 'name': entry['name'], 'address': f"0x{entry['vram']:08X}",
                                   'section': section['name'], 'protected': entry['name'] in protected})
                    if len(result) >= limit: return result
    return result


def bundle(output, tool, linux_x64=None, linux_arm64=None):
    files = {}
    for path in sorted((ROOT/'modding').rglob('*')):
        if path.is_file():
            files[path.relative_to(ROOT).as_posix()] = path
    for name in ('build_mod.py','rocket_sdk.py'):
        files['scripts/'+name] = ROOT/'scripts'/name
    for name in ('rocket.functions.dump.toml','rocket.data.dump.toml'):
        files['symbols/'+name] = ROOT/'build/generated'/name
    for name in ('modding.md','SDK2.md'):
        files['docs/'+name] = ROOT/'docs'/name
    for name in ('LICENSE','LICENSE.md','THIRD_PARTY.md'):
        files[name] = ROOT/name
    files['licenses/N64Recomp-LICENSE'] = ROOT/'extern/n64-modern-runtime/N64Recomp/LICENSE'
    from stage_release_docs import release_documents
    files.update({name: path for name, path in release_documents(ROOT).items()
                  if name.startswith('licenses/n64-modern-runtime/')})
    files['tools/'+tool.name] = tool
    for arch, native in (('x86_64', linux_x64), ('aarch64', linux_arm64)):
        if native: files[f'tools/linux-{arch}/RecompModTool'] = native
    require(all(p.is_file() for p in files.values()), 'SDK files or matching symbol dumps are missing')
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        for name,path in sorted(files.items()):
            info = zipfile.ZipInfo(name,(2026,1,1,0,0,0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100755 if name.startswith('tools/') and not name.endswith('.exe') else 0o100644) << 16
            archive.writestr(info,path.read_bytes())
        archive.writestr('sdk.json',json.dumps({'sdk':2,'host_minimum':'1.0.2','modules':MODULES,
            'symbols_sha256':{n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in files.items() if n.startswith('symbols/')}},indent=2)+'\n')
        protected = re.findall(r'// (\w+)', (ROOT/'generated/mod_protection.generated.hpp').read_text())
        archive.writestr('symbols/protected-functions.json', json.dumps(protected, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command',required=True)
    p = sub.add_parser('init');p.add_argument('project',type=Path);p.add_argument('--id',required=True)
    p = sub.add_parser('validate');p.add_argument('package',type=Path)
    p = sub.add_parser('build');p.add_argument('project',type=Path);p.add_argument('--wsl',action='store_true');p.add_argument('--output',type=Path,default=Path('build/mods'));p.add_argument('--tool',type=Path);p.add_argument('--symbols',type=Path);p.add_argument('--data-symbols',type=Path)
    p = sub.add_parser('asset-patch');p.add_argument('source',type=Path);p.add_argument('target',type=Path);p.add_argument('output',type=Path)
    p = sub.add_parser('mesh');p.add_argument('source',type=Path);p.add_argument('output',type=Path);p.add_argument('--colour',default='FFFFFFFF');p.add_argument('--y-up',action='store_true');p.add_argument('--texture',type=Path)
    p = sub.add_parser('bundle');p.add_argument('output',type=Path);p.add_argument('--tool',type=Path,required=True);p.add_argument('--linux-x64-tool',type=Path);p.add_argument('--linux-arm64-tool',type=Path)
    p = sub.add_parser('symbols');p.add_argument('pattern');p.add_argument('--kind',choices=('all','function','data'),default='all');p.add_argument('--limit',type=int,default=50)
    args = parser.parse_args()
    if args.command == 'init':
        initialise(args.project,args.id)
    elif args.command == 'validate':
        print(json.dumps(validate_package(args.package),indent=2))
    elif args.command == 'asset-patch':
        patch = make_asset_patch(args.source.read_bytes(),args.target.read_bytes());validate_asset_patch(patch)
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(patch)
        print(f'{len(patch)} bytes: {args.output}')
    elif args.command == 'mesh':
        data=compile_mesh(args.source,args.colour,args.y_up,args.texture)
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(data)
    elif args.command == 'bundle':
        bundle(args.output,args.tool,args.linux_x64_tool,args.linux_arm64_tool)
    elif args.command == 'symbols':
        print(json.dumps(symbol_search(args.pattern,args.kind,args.limit),indent=2))
    elif args.command == 'build':
        bundled = ROOT/'symbols/rocket.functions.dump.toml'
        symbols = args.symbols or (bundled if bundled.exists() else ROOT/'build/generated/rocket.functions.dump.toml')
        data = args.data_symbols or symbols.with_name('rocket.data.dump.toml')
        default_tool = ROOT/'tools'/('RecompModTool.exe' if sys.platform == 'win32' else 'RecompModTool')
        if sys.platform.startswith('linux'):
            native = ROOT/'tools'/('linux-'+platform.machine())/'RecompModTool'
            if native.exists(): default_tool = native
        tool = args.tool or (default_tool if default_tool.exists() else ROOT/'build/windows/N64ModernRuntime/librecomp/N64Recomp/RecompModTool.exe')
        command = [sys.executable,str(ROOT/'scripts/build_mod.py'),str(args.project),'--tool',str(tool),'--symbols',str(symbols),'--data-symbols',str(data),'--output',str(args.output)]
        if args.wsl: command.append('--wsl')
        subprocess.run(command,check=True)
        mod_id = tomllib.loads((args.project/'mod.toml').read_text(encoding='utf-8'))['manifest']['id']
        print(json.dumps(validate_package(args.output/(mod_id+'.nrm')),indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError,KeyError,OSError,zipfile.BadZipFile,subprocess.CalledProcessError) as error:
        print(f'ERROR: {error}',file=sys.stderr)
        sys.exit(1)
