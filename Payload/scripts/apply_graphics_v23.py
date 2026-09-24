#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, datetime, shutil, subprocess, sys

SELF_CHECK_V23 = r'''    # v23: recover Rocket's authored submodel visibility-mask rejection while preserving
    # the sane viewport-aware forward frustum, r7 render distance/fade and v21 queue recovery.
    _v23_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v23_culling = (_v23_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v23_graphics = (_v23_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v23_presentation = (_v23_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v23_oneclick = (_v23_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v23_queue_path = _v23_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    _v23_visibility_path = _v23_root / 'scripts' / 'patch_visibility_retention_v23_generated.py'
    require(_v23_queue_path.is_file(), 'v21 generated queue patcher missing')
    require(_v23_visibility_path.is_file(), 'v23 generated visibility-retention patcher missing')
    _v23_queue = _v23_queue_path.read_text(encoding='utf-8-sig')
    _v23_visibility = _v23_visibility_path.read_text(encoding='utf-8-sig')
    _v23_required = (
        'v16 viewport-locked FOV/aspect guard active' in _v23_culling and
        'rocket_popdiag_frustum_call()' in _v23_culling and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v23_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v23_graphics and
        's.draw_distance_multiplier' in _v23_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v23_presentation and
        'rocket_render_queue_prepare_first_batch' in _v23_presentation and
        'rocket_render_queue_prepare_next_batch' in _v23_presentation and
        'rocket_popdiag_visibility_mask_result' in _v23_presentation and
        'authored-submodel-mask=RECOVERED-V23' in _v23_presentation and
        'queue-overflow=RECOVERED-V21' in _v23_presentation and
        'patch_popin_diagnostics_generated.py' in _v23_oneclick and
        'patch_render_queue_expansion_v21_generated.py' in _v23_oneclick and
        'patch_visibility_retention_v23_generated.py' in _v23_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v23_queue and
        'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' in _v23_visibility
    )
    _v23_forbidden = any(token in (_v23_presentation + _v23_oneclick) for token in (
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
    ))
    if not (_v23_required and not _v23_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v23 authored-visibility retention + v21 queue recovery state missing')
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
    depth = 0
    state = 'code'
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state = 'line'; i += 2; continue
            if c == '/' and n == '*': state = 'block'; i += 2; continue
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return start, i + 1
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
        raise RuntimeError('Expected v22 FLT_MAX side-plane bypass or v21 viewport guard; refusing ambiguous culling edit')
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
    counter = 'std::atomic<std::uint64_t> g_popdiag_visibility_mask_recovered{0U};'
    if counter not in text:
        anchor = 'std::atomic<std::uint64_t> g_popdiag_frame_serial{0U};'
        if anchor not in text:
            raise RuntimeError('Pop-in telemetry counter anchor missing')
        text = text.replace(anchor, anchor + '\n' + counter, 1)
        changed = True

    hook = '''extern "C" void rocket_popdiag_visibility_mask_result(recomp_context* context) {
    if (context != nullptr && context->r2 == 0) {
        g_popdiag_visibility_mask_recovered.fetch_add(1U, std::memory_order_relaxed);
    }
}

'''
    if 'extern "C" void rocket_popdiag_visibility_mask_result' not in text:
        anchor = '''extern "C" void rocket_popdiag_frustum_call(void) {
    g_popdiag_frustum_calls.fetch_add(1U, std::memory_order_relaxed);
}

'''
        if anchor not in text:
            raise RuntimeError('Frustum telemetry hook anchor missing')
        text = text.replace(anchor, anchor + hook, 1)
        changed = True

    exchange = '''    const std::uint64_t mask_recovered =
        g_popdiag_visibility_mask_recovered.exchange(0U, std::memory_order_relaxed);
'''
    if 'g_popdiag_visibility_mask_recovered.exchange' not in text:
        anchor = '''    const std::uint32_t max_entries =
        g_popdiag_max_guest_entries.exchange(0U, std::memory_order_relaxed);
'''
        if anchor not in text:
            raise RuntimeError('Pop-in frame flush anchor missing')
        text = text.replace(anchor, anchor + exchange, 1)
        changed = True

    for old in (
        '=== Rocket-R v22 NO-SIDE-CULL + QUEUE-RECOVERY diagnostic session ===',
        '=== Rocket-R v21 IN-FUNCTION QUEUE-EXPANSION diagnostic session ===',
        '=== Rocket-R v20.2 ORIGINAL-BASELINE pop-in diagnostic session ===',
    ):
        if old in text:
            text = text.replace(old, '=== Rocket-R v23 AUTHORED-VISIBILITY RETENTION + QUEUE-RECOVERY diagnostic session ===')
            changed = True

    if 'authored-submodel-mask=RECOVERED-V23' not in text:
        if 'side-plane-cpu-cull=DISABLED-V22' in text:
            text = text.replace(
                'side-plane-cpu-cull=DISABLED-V22',
                'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RECOVERED-V23',
                1)
            changed = True
        elif 'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD' in text:
            text = text.replace(
                'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD',
                'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RECOVERED-V23',
                1)
            changed = True
        else:
            raise RuntimeError('Could not establish v23 diagnostic mode label')

    old_fields = '"would_drop_at_256_recovered\\n");'
    new_fields = '"would_drop_at_256_recovered authored_mask_recovered\\n");'
    if old_fields in text:
        text = text.replace(old_fields, new_fields, 1)
        changed = True
    elif 'authored_mask_recovered\\n");' not in text:
        raise RuntimeError('Diagnostic fields anchor missing')

    old_fmt = '"max_guest_queue=%u would_drop_at_256_recovered=%llu\\n",'
    new_fmt = '"max_guest_queue=%u would_drop_at_256_recovered=%llu authored_mask_recovered=%llu\\n",'
    if old_fmt in text:
        text = text.replace(old_fmt, new_fmt, 1)
        changed = True
    elif 'authored_mask_recovered=%llu\\n",' not in text:
        raise RuntimeError('Diagnostic frame format anchor missing')

    old_args = '''        max_entries,
        static_cast<unsigned long long>(rejected));'''
    new_args = '''        max_entries,
        static_cast<unsigned long long>(rejected),
        static_cast<unsigned long long>(mask_recovered));'''
    if old_args in text:
        text = text.replace(old_args, new_args, 1)
        changed = True
    elif 'static_cast<unsigned long long>(mask_recovered));' not in text:
        raise RuntimeError('Diagnostic frame argument anchor missing')

    if changed:
        write_text(path, text, bom, nl)
    return changed


def patch_oneclick(path):
    text, bom, nl = read_text(path)
    marker = "Invoke-Python @((Join-Path $Root 'scripts\\patch_visibility_retention_v23_generated.py'),'--root',$Root)"
    if marker in text:
        return False
    anchor = "    Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_expansion_v21_generated.py'),'--root',$Root)"
    if anchor not in text:
        raise RuntimeError('v21 queue patcher hook missing from OneClickBuild.ps1')
    insertion = (anchor + '\n\n'
        '    # Graphics v23: recover the authored submodel visibility-mask reject BEFORE frustum_test.\n'
        '    # The viewport-aware forward frustum, r7 distance/fade and v21 queue recovery remain active.\n'
        '    ' + marker.strip() + '\n')
    text = text.replace(anchor, insertion, 1)
    write_text(path, text, bom, nl)
    return True


def patch_self_check(path):
    text, bom, nl = read_text(path)
    if "# v23: recover Rocket's authored submodel visibility-mask rejection" in text:
        return False
    starts = [
        text.find('    # v22: no camera-angle CPU side-plane rejection + v21 in-function queue recovery.'),
        text.find('    # v21: preserve v20.2 culling/distance policy'),
    ]
    start = next((x for x in starts if x >= 0), -1)
    end = text.find('    scan_checked_source(root)', start)
    if start < 0 or end < 0:
        raise RuntimeError('Current v21/v22 graphics self-check block not found')
    text = text[:start] + SELF_CHECK_V23 + text[end:]
    ast.parse(text)
    write_text(path, text, bom, nl)
    return True


def generated_source_available(root):
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        return False
    for path in out.glob('funcs_*.c'):
        try:
            if 'RECOMP_FUNC void func_8001ECEC' in path.read_text(encoding='utf-8-sig'):
                return True
        except Exception:
            pass
    return False


def verify(root):
    c = (root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g = (root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    p = (root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    o = (root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc = (root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    q = (root/'scripts/patch_render_queue_expansion_v21_generated.py').read_text(encoding='utf-8-sig')
    v = (root/'scripts/patch_visibility_retention_v23_generated.py').read_text(encoding='utf-8-sig')
    s, e = function_span(c, 'extern "C" void rocket_widescreen_frustum_begin')
    body = c[s:e]
    for tok in ('rocket_popdiag_frustum_call();', 'v16 viewport-locked FOV/aspect guard active', 'context->r6 = static_cast<gpr>'):
        if tok not in body:
            raise RuntimeError('v23 viewport-aware culling token missing: ' + tok)
    if 'kDisableCpuSidePlanesBits' in body:
        raise RuntimeError('v22 FLT_MAX side-plane bypass is still active')
    if 'context->r7' in body:
        raise RuntimeError('widescreen culling hook must not modify r7 render distance')
    for tok in ('static_cast<std::uint32_t>(context->r7)', 's.draw_distance_multiplier'):
        if tok not in g:
            raise RuntimeError('Draw Distance path missing: ' + tok)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN',
                'rocket_render_queue_prepare_first_batch', 'rocket_render_queue_prepare_next_batch',
                'rocket_popdiag_visibility_mask_result', 'authored-submodel-mask=RECOVERED-V23',
                'queue-overflow=RECOVERED-V21', 'authored_mask_recovered'):
        if tok not in p:
            raise RuntimeError('v23 presentation/queue token missing: ' + tok)
    for tok in ('patch_popin_diagnostics_generated.py', 'patch_render_queue_expansion_v21_generated.py',
                'patch_visibility_retention_v23_generated.py'):
        if tok not in o:
            raise RuntimeError('OneClick generated hook missing: ' + tok)
    if 'ROCKET_QUEUE_V21_PROCESS_BATCH' not in q:
        raise RuntimeError('v21 queue patcher integrity marker missing')
    if 'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' not in v:
        raise RuntimeError('v23 visibility patcher integrity marker missing')
    if "# v23: recover Rocket's authored submodel visibility-mask rejection" not in sc:
        raise RuntimeError('v23 self-check missing')
    for tok in ('ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder', 'rocket_render_queue_begin_batch(',
                'rocket_original_func_8008B694', 'rocket_capture_func_8008B694', 'rocket_draw_func_8008B694'):
        if tok in p + o:
            raise RuntimeError('Retired v18/v19 renderer token present: ' + tok)


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
        root/'src/widescreen_culling.cpp',
        root/'src/graphics_enhancements.cpp',
        root/'src/presentation_identity.cpp',
        root/'scripts/OneClickBuild.ps1',
        root/'scripts/self_check.py',
        root/'scripts/patch_popin_diagnostics_generated.py',
        root/'scripts/patch_render_queue_expansion_v21_generated.py',
        replacement_path,
    ]
    for path in required:
        if not path.is_file():
            raise RuntimeError('Required v21/v22 file missing: ' + str(path))

    target_patcher = root/'scripts/patch_visibility_retention_v23_generated.py'
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = root/'build'/'repair-backups'/f'graphics-v23-{stamp}'
    backup_files = [
        'src/widescreen_culling.cpp', 'src/presentation_identity.cpp',
        'scripts/OneClickBuild.ps1', 'scripts/self_check.py',
        'scripts/patch_visibility_retention_v23_generated.py',
        'runtime-recomp/RecompiledFuncs/funcs_21.c',
        'runtime-recomp/RecompiledFuncs/funcs_27.c',
    ]
    existed = {}
    for rel in backup_files:
        src = root/rel
        existed[rel] = src.is_file()
        if src.is_file():
            dst = backup/rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    print('[OK] Backup:', backup)

    try:
        shutil.copy2(payload/'patch_visibility_retention_v23_generated.py', target_patcher)
        replacement = replacement_path.read_text(encoding='utf-8')
        changed_cull = patch_culling(root/'src/widescreen_culling.cpp', replacement)
        patch_presentation(root/'src/presentation_identity.cpp')
        patch_oneclick(root/'scripts/OneClickBuild.ps1')
        patch_self_check(root/'scripts/self_check.py')
        verify(root)

        if generated_source_available(root):
            queue_patcher = root/'scripts/patch_render_queue_expansion_v21_generated.py'
            run_checked([sys.executable, str(queue_patcher), '--root', str(root)], root,
                        'v21 generated queue patch')
            run_checked([sys.executable, str(target_patcher), '--root', str(root)], root,
                        'v23 generated visibility patch')
            run_checked([sys.executable, str(queue_patcher), '--root', str(root), '--verify'], root,
                        'v21 generated queue verification')
            run_checked([sys.executable, str(target_patcher), '--root', str(root), '--verify'], root,
                        'v23 generated visibility verification')
        else:
            print('[INFO] Generated CPU files are not present yet; OneClickBuild will apply v21 then v23 immediately after N64Recomp generation.')

        run_checked([sys.executable, str(root/'scripts/self_check.py')], root,
                    'Rocket-R source self-check')
        print('[OK] v23 restored viewport-aware forward culling.' if changed_cull else '[OK] viewport-aware forward culling already restored.')
        print('[OK] v23 authored submodel visibility retention is installed before frustum_test.')
        print('[OK] v21 in-function >256 queue recovery remains active.')
    except Exception:
        print('[ERROR] v23 install failed; restoring pre-v23 files from backup.', file=sys.stderr)
        for rel in backup_files:
            dst = root/rel
            src = backup/rel
            if existed.get(rel):
                if src.is_file():
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, dst)
            elif dst.exists():
                try:
                    dst.unlink()
                except Exception:
                    pass
        raise


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('Graphics v23 patch failed: ' + str(exc), file=sys.stderr)
        raise
