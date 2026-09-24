#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, datetime, re, shutil, subprocess, sys

SELF_CHECK_V25 = r'''    # v25: recover the three shared pre-render object-gate rejects while preserving
    # Rocket's retail submodel mask, viewport-aware frustum, r7 distance/fade and v21 queue recovery.
    _v25_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v25_culling = (_v25_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v25_graphics = (_v25_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v25_presentation = (_v25_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v25_oneclick = (_v25_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v25_queue_path = _v25_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    _v25_gate_path = _v25_root / 'scripts' / 'patch_prerender_object_gate_v25_generated.py'
    require(_v25_queue_path.is_file(), 'v21 generated queue patcher missing')
    require(_v25_gate_path.is_file(), 'v25 generated object-gate patcher missing')
    _v25_queue = _v25_queue_path.read_text(encoding='utf-8-sig')
    _v25_gate = _v25_gate_path.read_text(encoding='utf-8-sig')
    _v25_required = (
        'v16 viewport-locked FOV/aspect guard active' in _v25_culling and
        'rocket_popdiag_frustum_call()' in _v25_culling and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v25_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v25_graphics and
        's.draw_distance_multiplier' in _v25_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v25_presentation and
        'rocket_render_queue_prepare_first_batch' in _v25_presentation and
        'rocket_render_queue_prepare_next_batch' in _v25_presentation and
        'rocket_popdiag_object_gate_result' in _v25_presentation and
        'pre-render-object-gate=RECOVERED-V25' in _v25_presentation and
        'authored-submodel-mask=RETAIL' in _v25_presentation and
        'queue-overflow=RECOVERED-V21' in _v25_presentation and
        'patch_popin_diagnostics_generated.py' in _v25_oneclick and
        'patch_render_queue_expansion_v21_generated.py' in _v25_oneclick and
        'patch_prerender_object_gate_v25_generated.py' in _v25_oneclick and
        'patch_visibility_retention_v23_generated.py' not in _v25_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v25_queue and
        '0x8001E974' in _v25_gate and '0x8001E97C' in _v25_gate and
        '0x800261E4' in _v25_gate and '0x800261EC' in _v25_gate and
        '0x8005073C' in _v25_gate and '0x80050744' in _v25_gate
    )
    _v25_forbidden = any(token in (_v25_presentation + _v25_oneclick) for token in (
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
    ))
    if not (_v25_required and not _v25_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v25 pre-render object-gate retention + v21 queue state missing')
'''


def read_text(path):
    raw = path.read_bytes()
    return raw.decode('utf-8-sig'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')


def write_text(path, text, bom, nl):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def function_span(text, marker):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError('Function marker not found: ' + marker)
    brace = text.find('{', start)
    if brace < 0:
        raise RuntimeError('Opening brace not found: ' + marker)
    depth = 0; state = 'code'; i = brace
    while i < len(text):
        c = text[i]; n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state = 'line'; i += 2; continue
            if c == '/' and n == '*': state = 'block'; i += 2; continue
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0: return start, i + 1
        elif state == 'line':
            if c == '\n': state = 'code'
        elif state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 2; continue
        elif state == 'string':
            if c == '\\': i += 2; continue
            if c == '"': state = 'code'
        elif state == 'char':
            if c == '\\': i += 2; continue
            if c == "'": state = 'code'
        i += 1
    raise RuntimeError('Unterminated function: ' + marker)


def patch_culling(path, replacement):
    text, bom, nl = read_text(path)
    s, e = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    cur = text[s:e]
    if 'v16 viewport-locked FOV/aspect guard active' in cur and 'kDisableCpuSidePlanesBits' not in cur:
        return False
    if 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in cur:
        raise RuntimeError('Expected v17.1/v21 viewport guard or v22 FLT_MAX bypass; refusing ambiguous culling edit')
    text = text[:s] + replacement + text[e:]
    write_text(path, text, bom, nl)
    return True


def patch_presentation(path):
    text, bom, nl = read_text(path)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN',
                'rocket_render_queue_prepare_first_batch',
                'rocket_render_queue_prepare_next_batch',
                'queue-overflow=RECOVERED-V21'):
        if tok not in text:
            raise RuntimeError('Required v21 queue state missing: ' + tok)

    changed = False

    # Current user state is v23. Repurpose that exact telemetry slot rather than
    # stacking another counter/hook on top of it. A v21-only fallback is supported too.
    if 'g_popdiag_visibility_mask_recovered' in text:
        text = text.replace('g_popdiag_visibility_mask_recovered', 'g_popdiag_object_gate_recovered')
        text = text.replace('rocket_popdiag_visibility_mask_result', 'rocket_popdiag_object_gate_result')
        text = text.replace('mask_recovered', 'object_gate_recovered')
        changed = True
    else:
        counter = 'std::atomic<std::uint64_t> g_popdiag_object_gate_recovered{0U};'
        if counter not in text:
            anchor = 'std::atomic<std::uint64_t> g_popdiag_frame_serial{0U};'
            if anchor not in text: raise RuntimeError('Pop-in telemetry counter anchor missing')
            text = text.replace(anchor, anchor + '\n' + counter, 1); changed = True
        hook = '''extern "C" void rocket_popdiag_object_gate_result(recomp_context* context) {
    if (context != nullptr && context->r2 == 0) {
        g_popdiag_object_gate_recovered.fetch_add(1U, std::memory_order_relaxed);
    }
}

'''
        if 'extern "C" void rocket_popdiag_object_gate_result' not in text:
            anchor = '''extern "C" void rocket_popdiag_frustum_call(void) {
    g_popdiag_frustum_calls.fetch_add(1U, std::memory_order_relaxed);
}

'''
            if anchor not in text: raise RuntimeError('Frustum telemetry hook anchor missing')
            text = text.replace(anchor, anchor + hook, 1); changed = True
        exchange = '''    const std::uint64_t object_gate_recovered =
        g_popdiag_object_gate_recovered.exchange(0U, std::memory_order_relaxed);
'''
        if 'g_popdiag_object_gate_recovered.exchange' not in text:
            anchor = '''    const std::uint32_t max_entries =
        g_popdiag_max_guest_entries.exchange(0U, std::memory_order_relaxed);
'''
            if anchor not in text: raise RuntimeError('Pop-in frame flush anchor missing')
            text = text.replace(anchor, anchor + exchange, 1); changed = True

    # Normalize telemetry wording from v21/v22/v23 to v25.
    for old in (
        '=== Rocket-R v23 AUTHORED-VISIBILITY RETENTION + QUEUE-RECOVERY diagnostic session ===',
        '=== Rocket-R v22 NO-SIDE-CULL + QUEUE-RECOVERY diagnostic session ===',
        '=== Rocket-R v21 IN-FUNCTION QUEUE-EXPANSION diagnostic session ===',
        '=== Rocket-R v20.2 ORIGINAL-BASELINE pop-in diagnostic session ===',
    ):
        if old in text:
            text = text.replace(old, '=== Rocket-R v25 PRE-RENDER OBJECT-GATE RETENTION + QUEUE-RECOVERY diagnostic session ===')
            changed = True

    modes = (
        'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RECOVERED-V23',
        'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD',
        'side-plane-cpu-cull=DISABLED-V22',
    )
    if 'pre-render-object-gate=RECOVERED-V25' not in text:
        for old in modes:
            if old in text:
                text = text.replace(old,
                    'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL pre-render-object-gate=RECOVERED-V25', 1)
                changed = True
                break
        else:
            raise RuntimeError('Could not establish v25 diagnostic mode label')

    # The generic mask_recovered -> object_gate_recovered rename above can turn the
    # old field name into authored_object_gate_recovered. Normalize either v23 spelling.
    for old_field in ('authored_mask_recovered', 'authored_object_gate_recovered'):
        if old_field in text:
            text = text.replace(old_field, 'object_gate_recovered')
            changed = True

    # v21-only fallback needs the extra field/format/argument.
    if 'object_gate_recovered\\n");' not in text:
        old = '"would_drop_at_256_recovered\\n");'
        new = '"would_drop_at_256_recovered object_gate_recovered\\n");'
        if old not in text: raise RuntimeError('Diagnostic fields anchor missing')
        text = text.replace(old, new, 1); changed = True
    if 'object_gate_recovered=%llu\\n",' not in text:
        old = '"max_guest_queue=%u would_drop_at_256_recovered=%llu\\n",'
        new = '"max_guest_queue=%u would_drop_at_256_recovered=%llu object_gate_recovered=%llu\\n",'
        if old not in text: raise RuntimeError('Diagnostic frame format anchor missing')
        text = text.replace(old, new, 1); changed = True
    if 'static_cast<unsigned long long>(object_gate_recovered));' not in text:
        old = '''        max_entries,
        static_cast<unsigned long long>(rejected));'''
        new = '''        max_entries,
        static_cast<unsigned long long>(rejected),
        static_cast<unsigned long long>(object_gate_recovered));'''
        if old not in text: raise RuntimeError('Diagnostic frame argument anchor missing')
        text = text.replace(old, new, 1); changed = True

    if changed:
        write_text(path, text, bom, nl)
    return changed


def patch_oneclick(path):
    text, bom, nl = read_text(path)
    changed = False
    # Remove v23's now-disproven generated hook and its two descriptive comments.
    lines = text.replace('\r\n', '\n').splitlines(True)
    out = []
    skip_comments = 0
    for line in lines:
        if 'Graphics v23: recover the authored submodel visibility-mask reject BEFORE frustum_test.' in line:
            skip_comments = 2
            changed = True
            continue
        if skip_comments:
            skip_comments -= 1
            if 'patch_visibility_retention_v23_generated.py' in line or line.lstrip().startswith('# The viewport-aware'):
                changed = True
                continue
            # If structure differed, keep unrelated content.
        if 'patch_visibility_retention_v23_generated.py' in line:
            changed = True
            continue
        out.append(line)
    text = ''.join(out)

    marker = "Invoke-Python @((Join-Path $Root 'scripts\\patch_prerender_object_gate_v25_generated.py'),'--root',$Root)"
    if marker not in text:
        anchor = "    Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_expansion_v21_generated.py'),'--root',$Root)"
        if anchor not in text:
            raise RuntimeError('v21 queue patcher hook missing from OneClickBuild.ps1')
        insertion = (anchor + '\n\n'
            '    # Graphics v25: recover the shared pre-render whole-object gate at the three\n'
            '    # model-render callsites. Retail submodel mask + frustum + r7 distance remain authoritative.\n'
            '    ' + marker.strip() + '\n')
        text = text.replace(anchor, insertion, 1)
        changed = True
    if changed:
        write_text(path, text, bom, nl)
    return changed


def patch_self_check(path):
    text, bom, nl = read_text(path)
    if '# v25: recover the three shared pre-render object-gate rejects' in text:
        return False
    starts = [
        text.find("    # v23: recover Rocket's authored submodel visibility-mask rejection"),
        text.find('    # v22: no camera-angle CPU side-plane rejection + v21 in-function queue recovery.'),
        text.find('    # v21: preserve v20.2 culling/distance policy'),
    ]
    start = next((x for x in starts if x >= 0), -1)
    end = text.find('    scan_checked_source(root)', start)
    if start < 0 or end < 0:
        raise RuntimeError('Current v21/v22/v23 graphics self-check block not found')
    text = text[:start] + SELF_CHECK_V25 + text[end:]
    ast.parse(text)
    write_text(path, text, bom, nl)
    return True


def generated_source_available(root):
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir(): return False
    needed = {'func_8001E954', 'func_800261D0', 'func_80050728'}
    found = set()
    for path in out.glob('funcs_*.c'):
        try: t = path.read_text(encoding='utf-8-sig')
        except Exception: continue
        for name in tuple(needed - found):
            if f'RECOMP_FUNC void {name}' in t: found.add(name)
    return found == needed


def verify(root):
    c = (root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g = (root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    p = (root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    o = (root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc = (root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    q = (root/'scripts/patch_render_queue_expansion_v21_generated.py').read_text(encoding='utf-8-sig')
    v = (root/'scripts/patch_prerender_object_gate_v25_generated.py').read_text(encoding='utf-8-sig')
    s, e = function_span(c, 'extern "C" void rocket_widescreen_frustum_begin')
    body = c[s:e]
    for tok in ('rocket_popdiag_frustum_call();', 'v16 viewport-locked FOV/aspect guard active', 'context->r6 = static_cast<gpr>'):
        if tok not in body: raise RuntimeError('v25 viewport-aware culling token missing: ' + tok)
    if 'kDisableCpuSidePlanesBits' in body: raise RuntimeError('v22 FLT_MAX side-plane bypass is still active')
    if 'context->r7' in body: raise RuntimeError('widescreen culling hook must not modify r7 render distance')
    for tok in ('static_cast<std::uint32_t>(context->r7)', 's.draw_distance_multiplier'):
        if tok not in g: raise RuntimeError('Draw Distance path missing: ' + tok)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN',
                'rocket_render_queue_prepare_first_batch', 'rocket_render_queue_prepare_next_batch',
                'rocket_popdiag_object_gate_result', 'pre-render-object-gate=RECOVERED-V25',
                'authored-submodel-mask=RETAIL', 'queue-overflow=RECOVERED-V21', 'object_gate_recovered'):
        if tok not in p: raise RuntimeError('v25 presentation/queue token missing: ' + tok)
    for tok in ('patch_popin_diagnostics_generated.py', 'patch_render_queue_expansion_v21_generated.py',
                'patch_prerender_object_gate_v25_generated.py'):
        if tok not in o: raise RuntimeError('OneClick generated hook missing: ' + tok)
    if 'patch_visibility_retention_v23_generated.py' in o:
        raise RuntimeError('Disproven v23 generated hook is still active in OneClickBuild.ps1')
    if 'ROCKET_QUEUE_V21_PROCESS_BATCH' not in q: raise RuntimeError('v21 queue patcher integrity marker missing')
    for tok in ('0x8001E974','0x8001E97C','0x800261E4','0x800261EC','0x8005073C','0x80050744'):
        if tok not in v: raise RuntimeError('v25 gate patcher missing exact target: ' + tok)
    if '# v25: recover the three shared pre-render object-gate rejects' not in sc:
        raise RuntimeError('v25 self-check missing')
    for tok in ('ExpandedRenderEntry','RocketBuildGlobalRenderOrder','rocket_render_queue_begin_batch(',
                'rocket_original_func_8008B694','rocket_capture_func_8008B694','rocket_draw_func_8008B694'):
        if tok in p + o: raise RuntimeError('Retired v18/v19 renderer token present: ' + tok)


def run_checked(cmd, cwd, label):
    print('+ ' + ' '.join(str(x) for x in cmd))
    cp = subprocess.run(cmd, cwd=str(cwd))
    if cp.returncode:
        raise RuntimeError(f'{label} failed with exit code {cp.returncode}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    payload = Path(__file__).resolve().parent
    replacement_path = payload/'widescreen_culling_v21_function.txt'

    required = [
        root/'src/widescreen_culling.cpp', root/'src/graphics_enhancements.cpp',
        root/'src/presentation_identity.cpp', root/'scripts/OneClickBuild.ps1',
        root/'scripts/self_check.py', root/'scripts/patch_popin_diagnostics_generated.py',
        root/'scripts/patch_render_queue_expansion_v21_generated.py', replacement_path,
        payload/'patch_prerender_object_gate_v25_generated.py',
    ]
    for path in required:
        if not path.is_file(): raise RuntimeError('Required current Rocket-R graphics file missing: ' + str(path))

    target_patcher = root/'scripts/patch_prerender_object_gate_v25_generated.py'
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = root/'build'/'repair-backups'/f'graphics-v25-{stamp}'

    backup_files = [
        'src/widescreen_culling.cpp','src/presentation_identity.cpp',
        'scripts/OneClickBuild.ps1','scripts/self_check.py',
        'scripts/patch_prerender_object_gate_v25_generated.py',
        'scripts/patch_visibility_retention_v23_generated.py',
    ]
    out = root/'runtime-recomp'/'RecompiledFuncs'
    if out.is_dir():
        for path in out.glob('funcs_*.c'):
            try: t = path.read_text(encoding='utf-8-sig')
            except Exception: continue
            if any(x in t for x in ('RECOMP_FUNC void func_8001E954','RECOMP_FUNC void func_800261D0',
                                    'RECOMP_FUNC void func_80050728','ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL')):
                backup_files.append(str(path.relative_to(root)).replace('\\','/'))
    backup_files = list(dict.fromkeys(backup_files))
    existed = {}
    for rel in backup_files:
        src = root/rel; existed[rel] = src.is_file()
        if src.is_file():
            dst = backup/rel; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src,dst)
    print('[OK] Backup:', backup)

    try:
        shutil.copy2(payload/'patch_prerender_object_gate_v25_generated.py', target_patcher)
        replacement = replacement_path.read_text(encoding='utf-8')
        changed_cull = patch_culling(root/'src/widescreen_culling.cpp', replacement)
        patch_presentation(root/'src/presentation_identity.cpp')
        patch_oneclick(root/'scripts/OneClickBuild.ps1')
        patch_self_check(root/'scripts/self_check.py')
        verify(root)

        if generated_source_available(root):
            queue_patcher = root/'scripts/patch_render_queue_expansion_v21_generated.py'
            run_checked([sys.executable,str(queue_patcher),'--root',str(root)],root,'v21 generated queue patch')
            run_checked([sys.executable,str(target_patcher),'--root',str(root)],root,'v25 generated object-gate patch')
            run_checked([sys.executable,str(queue_patcher),'--root',str(root),'--verify'],root,'v21 generated queue verification')
            run_checked([sys.executable,str(target_patcher),'--root',str(root),'--verify'],root,'v25 generated object-gate verification')
        else:
            print('[INFO] Generated CPU files are not present yet; OneClickBuild will apply v21 then v25 immediately after N64Recomp generation.')

        run_checked([sys.executable,str(root/'scripts/self_check.py')],root,'Rocket-R source self-check')
        print('[OK] v25 viewport-aware frustum remains active.' if not changed_cull else '[OK] v25 restored the viewport-aware frustum from the v22 bypass.')
        print('[OK] v23 authored-submodel-mask bypass is retired; retail submodel mask restored.')
        print('[OK] v25 recovers the shared pre-render object gate at exactly three renderer callsites.')
        print('[OK] r7 Draw Distance/fade and v21 >256 queue recovery remain active.')
    except Exception:
        print('[ERROR] v25 install failed; restoring pre-v25 files from backup.', file=sys.stderr)
        for rel in backup_files:
            dst = root/rel; src = backup/rel
            if existed.get(rel):
                if src.is_file(): dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
            elif dst.exists():
                try: dst.unlink()
                except Exception: pass
        raise


if __name__ == '__main__':
    try: main()
    except Exception as exc:
        print('Graphics v25 patch failed: ' + str(exc), file=sys.stderr)
        raise
