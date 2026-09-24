#!/usr/bin/env python3
from pathlib import Path
import argparse
import ast
import datetime
import re
import shutil

CULLING_REPLACEMENT = r'''extern "C" void rocket_popdiag_frustum_call(void);

extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // v20 diagnostic/fix: keep Rocket's normal renderer and its r7 render-distance
    // calculation, but remove the four CPU side-plane tests as a source of
    // camera-angle pop. In retail frustum_test, cullRadius/r6 is used only by
    // those four side-plane comparisons. FLT_MAX therefore leaves distance
    // cutoff/fade (r7) untouched and lets the GPU clip off-screen geometry.
    rocket_popdiag_frustum_call();
    constexpr std::uint32_t kDisableCpuSidePlanesBits = 0x7F7FFFFFU; // FLT_MAX
    context->r6 = static_cast<gpr>(kDisableCpuSidePlanesBits);
}'''

DIAG_BLOCK = r'''
// v20: direct pop-in telemetry. This does not reorder, replay, expand or replace
// Rocket's renderer. It only observes add_render_entry before the retail 256
// capacity gate and writes a dedicated diagnostic file.
std::atomic<std::uint64_t> g_popdiag_frustum_calls{0U};
std::atomic<std::uint64_t> g_popdiag_visible_attempts{0U};
std::atomic<std::uint64_t> g_popdiag_rejected_at_capacity{0U};
std::atomic<std::uint32_t> g_popdiag_max_guest_entries{0U};
std::atomic<std::uint64_t> g_popdiag_frame_serial{0U};

static void RocketPopDiagAppendToPath(const char* path, const char* line) {
    if (path == nullptr || line == nullptr) return;
    std::FILE* file = std::fopen(path, "ab");
    if (file == nullptr) return;
    std::fputs(line, file);
    std::fflush(file);
    std::fclose(file);
}

static void RocketPopDiagWrite(const char* format, ...) {
    char line[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    // Always write a copy in the process working directory.
    RocketPopDiagAppendToPath("Rocket-R-popin-diagnostics.log", line);

    // Also write a predictable TEMP copy so the log is easy to find on Windows.
    const char* temp = std::getenv("TEMP");
    if (temp == nullptr || *temp == '\0') {
        temp = std::getenv("TMPDIR");
    }
    if (temp != nullptr && *temp != '\0') {
        char path[1024]{};
#ifdef _WIN32
        std::snprintf(path, sizeof(path), "%s\\Rocket-R-popin-diagnostics.log", temp);
#else
        std::snprintf(path, sizeof(path), "%s/Rocket-R-popin-diagnostics.log", temp);
#endif
        RocketPopDiagAppendToPath(path, line);
    }
}

static void RocketPopDiagAtomicMax(std::atomic<std::uint32_t>& target,
                                   std::uint32_t value) {
    std::uint32_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

extern "C" void rocket_popdiag_frustum_call(void) {
    g_popdiag_frustum_calls.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void rocket_popdiag_add_render_entry_attempt(std::uint8_t* rdram,
                                                            recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        MEM_W(0x14, context->r29) & 0xFF);
    if (alpha == 0U) return;

    g_popdiag_visible_attempts.fetch_add(1U, std::memory_order_relaxed);

    constexpr std::uint32_t kQueueBase = 0x800ADB00U;
    constexpr std::uint32_t kQueueEndPointerAddress = 0x800AF300U;
    constexpr std::uint32_t kEntryBytes = 24U;
    constexpr std::uint32_t kCapacity = 256U;
    const gpr end_pointer_address = static_cast<gpr>(
        static_cast<std::int32_t>(kQueueEndPointerAddress));
    const std::uint32_t end_pointer = static_cast<std::uint32_t>(
        MEM_W(0, end_pointer_address));

    if (end_pointer >= kQueueBase) {
        const std::uint32_t delta = end_pointer - kQueueBase;
        if ((delta % kEntryBytes) == 0U) {
            const std::uint32_t entries = delta / kEntryBytes;
            RocketPopDiagAtomicMax(g_popdiag_max_guest_entries,
                                   std::min(entries, kCapacity));
            if (entries >= kCapacity) {
                g_popdiag_rejected_at_capacity.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        }
    }
}

static void RocketPopDiagFlushPreviousFrame(void) {
    const std::uint64_t frame =
        g_popdiag_frame_serial.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const std::uint64_t frustum =
        g_popdiag_frustum_calls.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t attempts =
        g_popdiag_visible_attempts.exchange(0U, std::memory_order_relaxed);
    const std::uint64_t rejected =
        g_popdiag_rejected_at_capacity.exchange(0U, std::memory_order_relaxed);
    const std::uint32_t max_entries =
        g_popdiag_max_guest_entries.exchange(0U, std::memory_order_relaxed);

    if (frame == 1U) {
        RocketPopDiagWrite(
            "=== Rocket-R v20 pop-in diagnostic session ===\n"
            "mode=normal-v17.1-renderer side-plane-cpu-cull=DISABLED "
            "draw-distance=r7-preserved retail-distance-fade=preserved\n"
            "fields: frame frustum_calls visible_add_attempts max_guest_queue "
            "rejected_at_256\n");
    }

    // One compact line per authored frame. This remains small enough for a
    // short reproduction and makes camera-angle transitions easy to correlate.
    RocketPopDiagWrite(
        "frame=%llu frustum_calls=%llu visible_add_attempts=%llu "
        "max_guest_queue=%u rejected_at_256=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(frustum),
        static_cast<unsigned long long>(attempts),
        max_entries,
        static_cast<unsigned long long>(rejected));
}
'''


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n': text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom: data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0: raise RuntimeError(f'Function marker not found: {marker}')
    brace = text.find('{', start)
    if brace < 0: raise RuntimeError(f'Opening brace not found: {marker}')
    depth=0; state='code'; i=brace
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c=='"': state='string'
            elif c=="'": state='char'
            elif c=='{': depth+=1
            elif c=='}':
                depth-=1
                if depth==0: return start,i+1
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state=='string':
            if c=='\\': i+=2; continue
            if c=='"': state='code'
        elif state=='char':
            if c=='\\': i+=2; continue
            if c=="'": state='code'
        i+=1
    raise RuntimeError(f'Unterminated function: {marker}')


def patch_culling(path: Path):
    text,nl,bom=read_text(path)
    if ('v16 viewport-locked FOV/aspect guard active' not in text and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in text):
        raise RuntimeError('Recovered v17.1 or already-applied v20 culling baseline not found')
    # Remove a previous declaration if rerun, then replace function canonically.
    text = text.replace('extern "C" void rocket_popdiag_frustum_call(void);\n\n','')
    s,e=function_span(text,'extern "C" void rocket_widescreen_frustum_begin')
    text=text[:s]+CULLING_REPLACEMENT+text[e:]
    write_text(path,text,nl,bom)


def patch_presentation(path: Path):
    text,nl,bom=read_text(path)
    if 'ExpandedRenderEntry' in text or 'RocketBuildGlobalRenderOrder' in text or 'rocket_render_queue_begin_batch(' in text:
        raise RuntimeError('Retired v18/v19 render replay tokens found; run v19.5 recovery first')
    if '#include <cstdarg>' not in text:
        anchor='#include <cstdio>\n'
        if anchor not in text: raise RuntimeError('presentation_identity.cpp <cstdio> include anchor missing')
        text=text.replace(anchor,anchor+'#include <cstdarg>\n#include <cstdlib>\n',1)
    elif '#include <cstdlib>' not in text:
        text=text.replace('#include <cstdarg>\n','#include <cstdarg>\n#include <cstdlib>\n',1)

    marker='// v20: direct pop-in telemetry.'
    if marker not in text:
        anchor='extern "C" void rocket_presentation_frame_begin'
        pos=text.find(anchor)
        if pos<0: raise RuntimeError('rocket_presentation_frame_begin anchor missing')
        text=text[:pos]+DIAG_BLOCK+'\n'+text[pos:]

    s,e=function_span(text,'extern "C" void rocket_presentation_frame_begin')
    body=text[s:e]
    call='    RocketPopDiagFlushPreviousFrame();\n'
    if call not in body:
        lock='    std::scoped_lock lock(g_mutex);\n'
        if lock not in body: raise RuntimeError('frame_begin lock anchor missing')
        body=body.replace(lock,lock+call,1)
        text=text[:s]+body+text[e:]
    write_text(path,text,nl,bom)


def patch_oneclick(path: Path):
    text,nl,bom=read_text(path)
    token='patch_popin_diagnostics_generated.py'
    if token in text:
        return

    normalized = text.replace('\r\n', '\n')
    lines = normalized.splitlines(True)
    start = next((i for i,line in enumerate(lines) if 'if ($recompExit -ne 0)' in line), None)
    if start is None:
        raise RuntimeError('OneClick N64Recomp generation failure-check anchor missing')

    depth = 0
    end = None
    seen_open = False
    for i in range(start, min(len(lines), start + 20)):
        line = lines[i]
        opens = line.count('{')
        closes = line.count('}')
        if opens:
            seen_open = True
        depth += opens - closes
        if seen_open and depth == 0:
            end = i + 1
            break
    if end is None:
        raise RuntimeError('Could not locate end of N64Recomp failure-check block')

    indent = re.match(r'[ \t]*', lines[start]).group(0)
    insertion = (
        '\n' + indent + '# Graphics v20: diagnostic-only instrumentation. This adds one observation\n' +
        indent + '# call at generated add_render_entry entry; it does not wrap or replace the renderer.\n' +
        indent + "Invoke-Python @((Join-Path $Root 'scripts\\patch_popin_diagnostics_generated.py'),'--root',$Root)\n"
    )
    lines[end:end] = [insertion]
    text = ''.join(lines)
    write_text(path,text,nl,bom)



def patch_self_check(path: Path):
    text,nl,bom=read_text(path)
    try:
        tree=ast.parse(text)
    except SyntaxError as exc:
        raise RuntimeError(f'self_check.py invalid before v20 migration: {exc}')
    main=next((n for n in tree.body if isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef)) and n.name=='main'),None)
    if main is None:
        raise RuntimeError('self_check.py main() missing')

    lines=text.splitlines(keepends=True)
    ranges=[]
    for stmt in main.body:
        seg=ast.get_source_segment(text,stmt) or ''
        names={n.id for n in ast.walk(stmt) if isinstance(n,ast.Name)}
        if any(name.startswith('_v171') or name.startswith('_v20') for name in names) or 'v19.5 recovery' in seg or 'SOURCE SELF-CHECK FAILED: v19.5' in seg or 'SOURCE SELF-CHECK FAILED: v20' in seg:
            ranges.append((stmt.lineno-1,stmt.end_lineno))
    for first,last in sorted(ranges,reverse=True):
        del lines[first:last]
    text=''.join(lines)
    text=text.replace('    # v19.5 recovery: proven v16/v17.1 renderer baseline; no v18/v19 queue replay.\n','')
    tree=ast.parse(text)
    main=next(n for n in tree.body if isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef)) and n.name=='main')
    lines=text.splitlines(keepends=True)
    insert_line=None
    for stmt in main.body:
        seg=ast.get_source_segment(text,stmt) or ''
        if 'scan_checked_source(root)' in seg:
            insert_line=stmt.lineno-1
            break
    if insert_line is None:
        for stmt in main.body:
            if isinstance(stmt,ast.Return):
                insert_line=stmt.lineno-1; break
    if insert_line is None:
        insert_line=main.end_lineno
    indent='    '
    if main.body:
        indent=re.match(r'[ \t]*',lines[main.body[0].lineno-1]).group(0)
    raw='''# v20: normal v17.1 renderer + distance-only visibility + direct queue telemetry.
_v20_root = __import__('pathlib').Path(__file__).resolve().parents[1]
_v20_culling = (_v20_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
_v20_graphics = (_v20_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
_v20_presentation = (_v20_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
_v20_oneclick = (_v20_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
_v20_required = (
    'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' in _v20_culling and
    'rocket_popdiag_frustum_call()' in _v20_culling and
    'context->r6 = static_cast<gpr>' in _v20_culling and
    'context->r7' not in _v20_culling and
    'static_cast<std::uint32_t>(context->r7)' in _v20_graphics and
    's.draw_distance_multiplier' in _v20_graphics and
    'rocket_popdiag_add_render_entry_attempt' in _v20_presentation and
    'Rocket-R-popin-diagnostics.log' in _v20_presentation and
    'patch_popin_diagnostics_generated.py' in _v20_oneclick
)
_v20_forbidden = any(token in (_v20_presentation + _v20_oneclick) for token in (
    'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
    'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
    '[render-queue] GLOBAL', '[render-queue] EXPANDED',
)) or 'maximum_detail' in _v20_graphics or 'MEM_W(0x14' in _v20_graphics
if not (_v20_required and not _v20_forbidden):
    raise SystemExit('SOURCE SELF-CHECK FAILED: v20 culling-only/telemetry baseline missing')
'''
    block=''.join(indent+line if line.strip() else line for line in raw.splitlines(True))
    lines[insert_line:insert_line]=[block]
    text=''.join(lines)
    ast.parse(text)
    write_text(path,text,nl,bom)

def verify(root: Path):
    c=(root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    p=(root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    g=(root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    o=(root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc=(root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    if 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in c or 'context->r6 = static_cast<gpr>' not in c:
        raise RuntimeError('side-plane bypass missing')
    if 'context->r7' in c:
        raise RuntimeError('culling hook must not modify r7')
    gs,ge=function_span(g,'extern "C" void rocket_graphics_frustum_begin')
    gb=g[gs:ge]
    for tok in ('static_cast<std::uint32_t>(context->r7)','s.draw_distance_multiplier','context->r7 ='):
        if tok not in gb: raise RuntimeError('Draw Distance r7 baseline missing: '+tok)
    for tok in ('rocket_popdiag_add_render_entry_attempt','RocketPopDiagFlushPreviousFrame','rejected_at_256','Rocket-R-popin-diagnostics.log'):
        if tok not in p: raise RuntimeError('diagnostic source token missing: '+tok)
    if 'patch_popin_diagnostics_generated.py' not in o:
        raise RuntimeError('persistent generated diagnostic hook missing from OneClickBuild')
    if '# v20: normal v17.1 renderer + distance-only visibility + direct queue telemetry.' not in sc:
        raise RuntimeError('v20 self-check block missing')
    for tok in ('ExpandedRenderEntry','RocketBuildGlobalRenderOrder','rocket_render_queue_begin_batch('):
        if tok in p+o: raise RuntimeError('retired renderer token returned: '+tok)


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); ap.add_argument('--payload',required=True)
    args=ap.parse_args(); root=Path(args.root).resolve(); payload=Path(args.payload).resolve()
    files=[root/'src/widescreen_culling.cpp',root/'src/presentation_identity.cpp',root/'src/graphics_enhancements.cpp',root/'scripts/OneClickBuild.ps1',root/'scripts/self_check.py']
    for f in files:
        if not f.is_file(): raise RuntimeError('Required file missing: '+str(f))

    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup=root/'build'/'repair-backups'/f'graphics-v20-{stamp}'
    backup.mkdir(parents=True,exist_ok=True)
    for f in files:
        rel=f.relative_to(root); dst=backup/rel; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(f,dst)
    gen=root/'runtime-recomp'/'RecompiledFuncs'
    if gen.is_dir():
        for src in gen.glob('*.c'):
            try: t=src.read_text(encoding='utf-8-sig')
            except UnicodeDecodeError: continue
            if 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)' in t:
                dst=backup/src.relative_to(root); dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    diag_target = root/'scripts'/'patch_popin_diagnostics_generated.py'
    diag_existed = diag_target.exists()
    if diag_existed:
        dst=backup/diag_target.relative_to(root); dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(diag_target,dst)
    print('[OK] Backup:', backup)

    try:
        patch_culling(files[0]); patch_presentation(files[1]); patch_oneclick(files[3]); patch_self_check(files[4])
        shutil.copy2(payload/'patch_popin_diagnostics_generated.py',diag_target)
        verify(root)
    except Exception:
        for f in files:
            src=backup/f.relative_to(root)
            if src.is_file(): shutil.copy2(src,f)
        if diag_existed:
            src=backup/diag_target.relative_to(root)
            if src.is_file(): shutil.copy2(src,diag_target)
        elif diag_target.exists():
            diag_target.unlink()
        raise
    print('[OK] v20 culling-only policy installed: r6 side-plane rejection disabled; r7 Draw Distance/fade preserved.')
    print('[OK] Normal v17.1 renderer remains untouched.')
    print('[OK] Dedicated pop-in telemetry source installed.')

if __name__=='__main__': main()
