#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, datetime, re, shutil

CULLING_REPLACEMENT = 'extern "C" void rocket_popdiag_frustum_call(void);\n\nextern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,\n                                                   recomp_context* context) {\n    if (rdram == nullptr || context == nullptr) return;\n    rocket_popdiag_frustum_call();\n\n    // Rocket\'s Vec3f is typedef float Vec3f[3]. In a C function parameter it\n    // decays to float*, so the real o32 layout for frustum_test is:\n    //   a0/r4 = camera pointer\n    //   a1/r5 = position pointer\n    //   a2/r6 = cullRadius float bits\n    //   a3/r7 = renderDistance float bits\n    //   sp+0x10 = arg4 pointer\n    //   sp+0x14 = alphaOut pointer\n    // v15 incorrectly treated r5/r6/r7 as an inline XYZ vector. Do not do that.\n    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);\n    const std::uint32_t position_address = static_cast<std::uint32_t>(context->r5);\n    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes ||\n        position_address < kRdramStart || position_address > kRdramEnd - 12U) {\n        return;\n    }\n\n    const float authored_radius = std::bit_cast<float>(\n        static_cast<std::uint32_t>(context->r6));\n    if (!std::isfinite(authored_radius) || authored_radius < 0.0F) return;\n\n    const gpr camera = GuestAddress(camera_address);\n    const gpr position_ptr = GuestAddress(position_address);\n\n    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);\n    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);\n    const float effective_fov_y =\n        rocket::graphics::effective_fov_radians(authored_fov_y);\n    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||\n        !std::isfinite(authored_fov_y) || authored_fov_y <= 0.01F ||\n        authored_fov_y >= kPi - 0.01F ||\n        !std::isfinite(effective_fov_y) || effective_fov_y <= 0.01F ||\n        effective_fov_y >= kPi - 0.01F) {\n        return;\n    }\n\n    const float selected_aspect =\n        rocket::graphics::selected_aspect(authored_aspect);\n    if (!std::isfinite(selected_aspect) || selected_aspect <= 0.1F) return;\n\n    // The culling volume must follow the actual visible presentation. Never\n    // narrow retail visibility: aspect expansion widens left/right and positive\n    // FOV expansion widens BOTH vertical and horizontal coverage.\n    const float target_aspect = std::max(authored_aspect, selected_aspect);\n    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);\n    const bool aspect_expanded =\n        target_aspect > authored_aspect * 1.0001F;\n    const bool fov_expanded =\n        target_fov_y > authored_fov_y + 0.0001F;\n    if (!aspect_expanded && !fov_expanded) return;\n\n    const Vec3 eye = ReadVec3(rdram, camera, 0);\n    const Vec3 position = ReadVec3(rdram, position_ptr, 0);\n    if (!Finite(eye) || !Finite(position)) return;\n\n    const Vec3 camera_offset{\n        position.x - eye.x,\n        position.y - eye.y,\n        position.z - eye.z,\n    };\n    const float distance = Length(camera_offset);\n    if (!std::isfinite(distance)) return;\n\n    // Use Rocket\'s own live camera depth axis so the target frustum rotates\n    // with camera yaw/pitch/roll rather than remaining world-aligned.\n    Vec3 forward{};\n    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;\n\n    std::array<Vec3, 4> authored_planes{};\n    std::array<Vec3, 4> unit_planes{};\n    for (std::size_t index = 0; index < authored_planes.size(); ++index) {\n        authored_planes[index] = ReadVec3(\n            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);\n        if (!Finite(authored_planes[index])) return;\n        if (!Normalize(authored_planes[index], unit_planes[index])) {\n            return;\n        }\n    }\n\n    // Orient forward so visible objects in front of the camera lie on the\n    // accepted side of Rocket\'s four outward side-plane normals.\n    float forward_score = 0.0F;\n    for (const Vec3& plane : unit_planes) {\n        forward_score += Dot(plane, forward);\n    }\n    if (forward_score > 0.0F) {\n        forward = Scale(forward, -1.0F);\n    }\n\n    // Pair opposite planes geometrically; do not assume their storage order.\n    constexpr std::array<std::array<std::size_t, 4>, 3> kPairings{{\n        {{0U, 1U, 2U, 3U}},\n        {{0U, 2U, 1U, 3U}},\n        {{0U, 3U, 1U, 2U}},\n    }};\n\n    std::size_t pairing_index = 0U;\n    float best_pair_score = std::numeric_limits<float>::infinity();\n    for (std::size_t candidate = 0; candidate < kPairings.size(); ++candidate) {\n        const auto& p = kPairings[candidate];\n        const float score =\n            Dot(unit_planes[p[0]], unit_planes[p[1]]) +\n            Dot(unit_planes[p[2]], unit_planes[p[3]]);\n        if (score < best_pair_score) {\n            best_pair_score = score;\n            pairing_index = candidate;\n        }\n    }\n\n    const auto& pairing = kPairings[pairing_index];\n    const std::array<std::array<std::size_t, 2>, 2> plane_pairs{{\n        {{pairing[0], pairing[1]}},\n        {{pairing[2], pairing[3]}},\n    }};\n\n    std::array<float, 2> authored_half_fov{};\n    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {\n        const auto& pair = plane_pairs[pair_index];\n        const float forward_component = std::clamp(\n            0.5F * (std::fabs(Dot(unit_planes[pair[0]], forward)) +\n                    std::fabs(Dot(unit_planes[pair[1]], forward))),\n            0.0F, 1.0F);\n        authored_half_fov[pair_index] = std::asin(forward_component);\n        if (!std::isfinite(authored_half_fov[pair_index])) return;\n    }\n\n    // Rocket\'s authored projection is wider than 1:1, so the pair with the\n    // larger half-angle is left/right; the other pair is top/bottom.\n    const std::size_t horizontal_pair =\n        authored_half_fov[0] >= authored_half_fov[1] ? 0U : 1U;\n\n    // Recreate the visible target rectangle from the *actual* effective\n    // vertical FOV and selected aspect ratio. Applying the guard in tangent\n    // space keeps a real margin beyond every viewport edge at all FOVs.\n    constexpr float kTargetFrustumGuard = 1.20F;\n    float vertical_tangent = std::tan(target_fov_y * 0.5F);\n    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;\n    vertical_tangent *= kTargetFrustumGuard;\n\n    const float requested_vertical_half = std::atan(vertical_tangent);\n    const float requested_horizontal_half =\n        std::atan(vertical_tangent * target_aspect);\n    if (!std::isfinite(requested_vertical_half) ||\n        !std::isfinite(requested_horizontal_half)) {\n        return;\n    }\n\n    std::array<Vec3, 4> target_unit_planes{};\n    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {\n        const float requested_half =\n            pair_index == horizontal_pair\n                ? requested_horizontal_half\n                : requested_vertical_half;\n        const float target_half =\n            std::max(authored_half_fov[pair_index], requested_half);\n\n        for (const std::size_t index : plane_pairs[pair_index]) {\n            const float forward_component = Dot(unit_planes[index], forward);\n            const Vec3 transverse_raw = Add(\n                unit_planes[index],\n                Scale(forward, -forward_component));\n            Vec3 transverse{};\n            if (!Normalize(transverse_raw, transverse)) return;\n\n            const float signed_forward = std::copysign(\n                std::sin(target_half), forward_component);\n            target_unit_planes[index] = Add(\n                Scale(transverse, std::cos(target_half)),\n                Scale(forward, signed_forward));\n        }\n    }\n\n    // Exact sphere-vs-target-side-plane test using UNIT target planes. This is\n    // deliberately based on the target viewport, not a diagonal cone, so an\n    // object can never be rejected merely because it is near a visible corner.\n    const float edge_slack = std::max(0.75F, distance * 0.006F);\n    for (const Vec3& target_plane : target_unit_planes) {\n        const float plane_distance = Dot(camera_offset, target_plane);\n        if (!std::isfinite(plane_distance)) return;\n        if (plane_distance > authored_radius + edge_slack) {\n            return;\n        }\n    }\n\n    // The object sphere intersects the visible target rectangle. Relax ONLY\n    // this call\'s cullRadius enough for Rocket\'s original authored plane tests\n    // to accept it. r7/renderDistance remains completely untouched.\n    float required_radius = authored_radius;\n    for (const Vec3& authored_plane : authored_planes) {\n        const float plane_distance = Dot(camera_offset, authored_plane);\n        if (!std::isfinite(plane_distance)) return;\n        required_radius = std::max(required_radius, plane_distance);\n    }\n    if (required_radius <= authored_radius) return;\n\n    const float adjusted_radius = required_radius + edge_slack;\n    if (!std::isfinite(adjusted_radius) || adjusted_radius <= 0.0F ||\n        adjusted_radius >= 1.0e+20F) {\n        return;\n    }\n\n    context->r6 = static_cast<gpr>(\n        std::bit_cast<std::uint32_t>(adjusted_radius));\n\n    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {\n        std::fprintf(stderr,\n            "[culling] v16 viewport-locked FOV/aspect guard active; "\n            "position=r5 pointer, cullRadius=r6, renderDistance=r7 untouched "\n            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, 20%% guard)\\n",\n            authored_aspect, target_aspect,\n            authored_fov_y * (180.0F / kPi),\n            target_fov_y * (180.0F / kPi));\n    }\n}'


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
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
            if c == '/' and n == '/':
                state = 'line'; i += 2; continue
            if c == '/' and n == '*':
                state = 'block'; i += 2; continue
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


def patch_culling(path: Path):
    text, nl, bom = read_text(path)
    if ('kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in text and
            'v16 viewport-locked FOV/aspect guard active' not in text):
        raise RuntimeError('Expected v20 or recovered v17.1 culling baseline not found')
    text = text.replace('extern "C" void rocket_popdiag_frustum_call(void);\n\n', '')
    s, e = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    text = text[:s] + CULLING_REPLACEMENT + text[e:]
    write_text(path, text, nl, bom)


def patch_presentation(path: Path):
    text, nl, bom = read_text(path)
    for tok in ('rocket_popdiag_add_render_entry_attempt', 'RocketPopDiagFlushPreviousFrame', 'Rocket-R-popin-diagnostics.log'):
        if tok not in text:
            raise RuntimeError('v20 telemetry source missing: ' + tok)
    text = text.replace('=== Rocket-R v20 pop-in diagnostic session ===',
                        '=== Rocket-R v20.2 ORIGINAL-BASELINE pop-in diagnostic session ===')
    text = text.replace('side-plane-cpu-cull=DISABLED',
                        'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD')
    write_text(path, text, nl, bom)


def patch_oneclick(path: Path):
    text, nl, bom = read_text(path)
    if 'patch_popin_diagnostics_generated.py' in text:
        return
    normalized = text.replace('\r\n', '\n')
    lines = normalized.splitlines(True)
    start = next((i for i, line in enumerate(lines) if 'if ($recompExit -ne 0)' in line), None)
    if start is None:
        raise RuntimeError('OneClick N64Recomp generation failure-check anchor missing')
    depth = 0; end = None; seen = False
    for i in range(start, min(len(lines), start + 20)):
        opens = lines[i].count('{'); closes = lines[i].count('}')
        if opens: seen = True
        depth += opens - closes
        if seen and depth == 0:
            end = i + 1; break
    if end is None:
        raise RuntimeError('Could not locate end of N64Recomp failure-check block')
    indent = re.match(r'[ \t]*', lines[start]).group(0)
    insertion = ('\n' + indent + '# Graphics v20.2: observe add_render_entry before the retail 256-entry gate.\n' +
                 indent + "Invoke-Python @((Join-Path $Root 'scripts\\patch_popin_diagnostics_generated.py'),'--root',$Root)\n")
    lines[end:end] = [insertion]
    write_text(path, ''.join(lines), nl, bom)


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)
    tree = ast.parse(text)
    main = next((n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if main is None:
        raise RuntimeError('self_check.py main() missing')
    lines = text.splitlines(keepends=True)
    ranges = []
    for stmt in main.body:
        seg = ast.get_source_segment(text, stmt) or ''
        names = {n.id for n in ast.walk(stmt) if isinstance(n, ast.Name)}
        if any(name.startswith('_v20') for name in names) or 'SOURCE SELF-CHECK FAILED: v20' in seg:
            ranges.append((stmt.lineno - 1, stmt.end_lineno))
    for first, last in sorted(ranges, reverse=True):
        del lines[first:last]
    text = ''.join(lines)
    tree = ast.parse(text)
    main = next(n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main')
    lines = text.splitlines(keepends=True)
    insert_line = None
    for stmt in main.body:
        seg = ast.get_source_segment(text, stmt) or ''
        if 'scan_checked_source(root)' in seg:
            insert_line = stmt.lineno - 1
            break
    if insert_line is None:
        for stmt in main.body:
            if isinstance(stmt, ast.Return):
                insert_line = stmt.lineno - 1
                break
    if insert_line is None:
        insert_line = main.end_lineno
    indent = re.match(r'[ \t]*', lines[main.body[0].lineno - 1]).group(0) if main.body else '    '
    raw = """# v20.2: recovered v17.1 viewport culling + direct pre-capacity queue telemetry.\n_v202_root = __import__('pathlib').Path(__file__).resolve().parents[1]\n_v202_culling = (_v202_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')\n_v202_graphics = (_v202_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')\n_v202_presentation = (_v202_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')\n_v202_oneclick = (_v202_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')\n_v202_required = (\n    'v16 viewport-locked FOV/aspect guard active' in _v202_culling and\n    'rocket_popdiag_frustum_call()' in _v202_culling and\n    'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v202_culling and\n    'static_cast<std::uint32_t>(context->r7)' in _v202_graphics and\n    's.draw_distance_multiplier' in _v202_graphics and\n    'rocket_popdiag_add_render_entry_attempt' in _v202_presentation and\n    'V17.1-VIEWPORT-GUARD' in _v202_presentation and\n    'patch_popin_diagnostics_generated.py' in _v202_oneclick\n)\n_v202_forbidden = any(token in (_v202_presentation + _v202_oneclick) for token in (\n    'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',\n    'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',\n    '[render-queue] GLOBAL', '[render-queue] EXPANDED',\n))\nif not (_v202_required and not _v202_forbidden):\n    raise SystemExit('SOURCE SELF-CHECK FAILED: v20.2 baseline-culling telemetry state missing')\n"""
    block = ''.join(indent + line if line.strip() else line for line in raw.splitlines(True))
    lines[insert_line:insert_line] = [block]
    text = ''.join(lines)
    ast.parse(text)
    write_text(path, text, nl, bom)


def verify(root: Path):
    c = (root / 'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    p = (root / 'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    g = (root / 'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    o = (root / 'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc = (root / 'scripts/self_check.py').read_text(encoding='utf-8-sig')
    if 'v16 viewport-locked FOV/aspect guard active' not in c:
        raise RuntimeError('v17.1 viewport culling not restored')
    if 'rocket_popdiag_frustum_call();' not in c:
        raise RuntimeError('frustum telemetry call missing')
    if 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' in c:
        raise RuntimeError('v20 side-plane bypass still active')
    if 'context->r7' in c:
        raise RuntimeError('widescreen culling hook must not modify r7')
    s, e = function_span(g, 'extern "C" void rocket_graphics_frustum_begin')
    body = g[s:e]
    for tok in ('static_cast<std::uint32_t>(context->r7)', 's.draw_distance_multiplier', 'context->r7 ='):
        if tok not in body:
            raise RuntimeError('Draw Distance r7 baseline missing: ' + tok)
    for tok in ('rocket_popdiag_add_render_entry_attempt', 'RocketPopDiagFlushPreviousFrame', 'V17.1-VIEWPORT-GUARD'):
        if tok not in p:
            raise RuntimeError('baseline telemetry token missing: ' + tok)
    if 'patch_popin_diagnostics_generated.py' not in o:
        raise RuntimeError('generated telemetry hook missing')
    if '# v20.2: recovered v17.1 viewport culling + direct pre-capacity queue telemetry.' not in sc:
        raise RuntimeError('v20.2 self-check block missing')
    for tok in ('ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder', 'rocket_render_queue_begin_batch('):
        if tok in p + o:
            raise RuntimeError('retired renderer token present: ' + tok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--payload', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve(); payload = Path(args.payload).resolve()
    files = [root/'src/widescreen_culling.cpp', root/'src/presentation_identity.cpp', root/'src/graphics_enhancements.cpp', root/'scripts/OneClickBuild.ps1', root/'scripts/self_check.py']
    for f in files:
        if not f.is_file():
            raise RuntimeError('Required file missing: ' + str(f))
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = root/'build'/'repair-backups'/f'graphics-v20.2-{stamp}'
    backup.mkdir(parents=True, exist_ok=True)
    for f in files:
        dst = backup/f.relative_to(root); dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(f, dst)
    diag = root/'scripts'/'patch_popin_diagnostics_generated.py'; existed = diag.exists()
    if existed:
        dst = backup/diag.relative_to(root); dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(diag, dst)
    print('[OK] Backup:', backup)
    try:
        patch_culling(files[0]); patch_presentation(files[1]); patch_oneclick(files[3]); patch_self_check(files[4])
        shutil.copy2(payload/'patch_popin_diagnostics_generated.py', diag)
        verify(root)
    except Exception:
        for f in files:
            src = backup/f.relative_to(root)
            if src.is_file(): shutil.copy2(src, f)
        if existed:
            src = backup/diag.relative_to(root)
            if src.is_file(): shutil.copy2(src, diag)
        elif diag.exists():
            diag.unlink()
        raise
    print('[OK] v20.2 restored recovered v17.1/v16 viewport culling; v20 FLT_MAX side-plane bypass removed.')
    print('[OK] Extended Draw Distance remains on r7 and Rocket retail distance fade remains active.')
    print('[OK] Reliable pre-capacity add_render_entry telemetry remains installed.')


if __name__ == '__main__':
    main()
