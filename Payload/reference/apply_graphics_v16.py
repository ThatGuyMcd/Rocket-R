#!/usr/bin/env python3
from pathlib import Path
import argparse
import ast
import re

CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // Rocket's Vec3f is typedef float Vec3f[3]. In a C function parameter it
    // decays to float*, so the real o32 layout for frustum_test is:
    //   a0/r4 = camera pointer
    //   a1/r5 = position pointer
    //   a2/r6 = cullRadius float bits
    //   a3/r7 = renderDistance float bits
    //   sp+0x10 = arg4 pointer
    //   sp+0x14 = alphaOut pointer
    // v15 incorrectly treated r5/r6/r7 as an inline XYZ vector. Do not do that.
    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    const std::uint32_t position_address = static_cast<std::uint32_t>(context->r5);
    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes ||
        position_address < kRdramStart || position_address > kRdramEnd - 12U) {
        return;
    }

    const float authored_radius = std::bit_cast<float>(
        static_cast<std::uint32_t>(context->r6));
    if (!std::isfinite(authored_radius) || authored_radius < 0.0F) return;

    const gpr camera = GuestAddress(camera_address);
    const gpr position_ptr = GuestAddress(position_address);

    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);
    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);
    const float effective_fov_y =
        rocket::graphics::effective_fov_radians(authored_fov_y);
    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||
        !std::isfinite(authored_fov_y) || authored_fov_y <= 0.01F ||
        authored_fov_y >= kPi - 0.01F ||
        !std::isfinite(effective_fov_y) || effective_fov_y <= 0.01F ||
        effective_fov_y >= kPi - 0.01F) {
        return;
    }

    const float selected_aspect =
        rocket::graphics::selected_aspect(authored_aspect);
    if (!std::isfinite(selected_aspect) || selected_aspect <= 0.1F) return;

    // The culling volume must follow the actual visible presentation. Never
    // narrow retail visibility: aspect expansion widens left/right and positive
    // FOV expansion widens BOTH vertical and horizontal coverage.
    const float target_aspect = std::max(authored_aspect, selected_aspect);
    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);
    const bool aspect_expanded =
        target_aspect > authored_aspect * 1.0001F;
    const bool fov_expanded =
        target_fov_y > authored_fov_y + 0.0001F;
    if (!aspect_expanded && !fov_expanded) return;

    const Vec3 eye = ReadVec3(rdram, camera, 0);
    const Vec3 position = ReadVec3(rdram, position_ptr, 0);
    if (!Finite(eye) || !Finite(position)) return;

    const Vec3 camera_offset{
        position.x - eye.x,
        position.y - eye.y,
        position.z - eye.z,
    };
    const float distance = Length(camera_offset);
    if (!std::isfinite(distance)) return;

    // Use Rocket's own live camera depth axis so the target frustum rotates
    // with camera yaw/pitch/roll rather than remaining world-aligned.
    Vec3 forward{};
    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;

    std::array<Vec3, 4> authored_planes{};
    std::array<Vec3, 4> unit_planes{};
    for (std::size_t index = 0; index < authored_planes.size(); ++index) {
        authored_planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Finite(authored_planes[index])) return;
        if (!Normalize(authored_planes[index], unit_planes[index])) {
            return;
        }
    }

    // Orient forward so visible objects in front of the camera lie on the
    // accepted side of Rocket's four outward side-plane normals.
    float forward_score = 0.0F;
    for (const Vec3& plane : unit_planes) {
        forward_score += Dot(plane, forward);
    }
    if (forward_score > 0.0F) {
        forward = Scale(forward, -1.0F);
    }

    // Pair opposite planes geometrically; do not assume their storage order.
    constexpr std::array<std::array<std::size_t, 4>, 3> kPairings{{
        {{0U, 1U, 2U, 3U}},
        {{0U, 2U, 1U, 3U}},
        {{0U, 3U, 1U, 2U}},
    }};

    std::size_t pairing_index = 0U;
    float best_pair_score = std::numeric_limits<float>::infinity();
    for (std::size_t candidate = 0; candidate < kPairings.size(); ++candidate) {
        const auto& p = kPairings[candidate];
        const float score =
            Dot(unit_planes[p[0]], unit_planes[p[1]]) +
            Dot(unit_planes[p[2]], unit_planes[p[3]]);
        if (score < best_pair_score) {
            best_pair_score = score;
            pairing_index = candidate;
        }
    }

    const auto& pairing = kPairings[pairing_index];
    const std::array<std::array<std::size_t, 2>, 2> plane_pairs{{
        {{pairing[0], pairing[1]}},
        {{pairing[2], pairing[3]}},
    }};

    std::array<float, 2> authored_half_fov{};
    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {
        const auto& pair = plane_pairs[pair_index];
        const float forward_component = std::clamp(
            0.5F * (std::fabs(Dot(unit_planes[pair[0]], forward)) +
                    std::fabs(Dot(unit_planes[pair[1]], forward))),
            0.0F, 1.0F);
        authored_half_fov[pair_index] = std::asin(forward_component);
        if (!std::isfinite(authored_half_fov[pair_index])) return;
    }

    // Rocket's authored projection is wider than 1:1, so the pair with the
    // larger half-angle is left/right; the other pair is top/bottom.
    const std::size_t horizontal_pair =
        authored_half_fov[0] >= authored_half_fov[1] ? 0U : 1U;

    // Recreate the visible target rectangle from the *actual* effective
    // vertical FOV and selected aspect ratio. Applying the guard in tangent
    // space keeps a real margin beyond every viewport edge at all FOVs.
    constexpr float kTargetFrustumGuard = 1.20F;
    float vertical_tangent = std::tan(target_fov_y * 0.5F);
    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;
    vertical_tangent *= kTargetFrustumGuard;

    const float requested_vertical_half = std::atan(vertical_tangent);
    const float requested_horizontal_half =
        std::atan(vertical_tangent * target_aspect);
    if (!std::isfinite(requested_vertical_half) ||
        !std::isfinite(requested_horizontal_half)) {
        return;
    }

    std::array<Vec3, 4> target_unit_planes{};
    for (std::size_t pair_index = 0; pair_index < plane_pairs.size(); ++pair_index) {
        const float requested_half =
            pair_index == horizontal_pair
                ? requested_horizontal_half
                : requested_vertical_half;
        const float target_half =
            std::max(authored_half_fov[pair_index], requested_half);

        for (const std::size_t index : plane_pairs[pair_index]) {
            const float forward_component = Dot(unit_planes[index], forward);
            const Vec3 transverse_raw = Add(
                unit_planes[index],
                Scale(forward, -forward_component));
            Vec3 transverse{};
            if (!Normalize(transverse_raw, transverse)) return;

            const float signed_forward = std::copysign(
                std::sin(target_half), forward_component);
            target_unit_planes[index] = Add(
                Scale(transverse, std::cos(target_half)),
                Scale(forward, signed_forward));
        }
    }

    // Exact sphere-vs-target-side-plane test using UNIT target planes. This is
    // deliberately based on the target viewport, not a diagonal cone, so an
    // object can never be rejected merely because it is near a visible corner.
    const float edge_slack = std::max(0.75F, distance * 0.006F);
    for (const Vec3& target_plane : target_unit_planes) {
        const float plane_distance = Dot(camera_offset, target_plane);
        if (!std::isfinite(plane_distance)) return;
        if (plane_distance > authored_radius + edge_slack) {
            return;
        }
    }

    // The object sphere intersects the visible target rectangle. Relax ONLY
    // this call's cullRadius enough for Rocket's original authored plane tests
    // to accept it. r7/renderDistance remains completely untouched.
    float required_radius = authored_radius;
    for (const Vec3& authored_plane : authored_planes) {
        const float plane_distance = Dot(camera_offset, authored_plane);
        if (!std::isfinite(plane_distance)) return;
        required_radius = std::max(required_radius, plane_distance);
    }
    if (required_radius <= authored_radius) return;

    const float adjusted_radius = required_radius + edge_slack;
    if (!std::isfinite(adjusted_radius) || adjusted_radius <= 0.0F ||
        adjusted_radius >= 1.0e+20F) {
        return;
    }

    context->r6 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted_radius));

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[culling] v16 viewport-locked FOV/aspect guard active; "
            "position=r5 pointer, cullRadius=r6, renderDistance=r7 untouched "
            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, 20%% guard)\n",
            authored_aspect, target_aspect,
            authored_fov_y * (180.0F / kPi),
            target_fov_y * (180.0F / kPi));
    }
}'''

V15_MESSAGE = (
    "FIXED34/v15 must use the real MIPS o32 frustum_test ABI and "
    "camera-forward-anchored object-local target-frustum culling"
)
OLD_MESSAGES = (
    "FIXED34/v14 must bypass Rocket CPU side-plane rejection in expanded presentation modes while preserving render-distance culling",
    "FIXED34/v13.1 must use exact object-local target-frustum culling derived from Rocket's authored four side planes",
    "FIXED34/v13 must use exact object-local target-frustum culling derived from Rocket's authored four side planes",
    "FIXED34/v12 must use object-local FOV/aspect culling, preserve the window-aspect bridge, and avoid rewriting shared camera frustum planes",
    "FIXED34 must widen only Rocket's horizontal CPU frustum in Expand mode with a guarded, idempotent window-aspect bridge",
)
V16_MESSAGE = (
    "FIXED34/v16 must use Rocket's pointer ABI and viewport-locked four-plane FOV/aspect culling"
)


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n":
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Function marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found: {marker}")
    depth = 0
    state = "code"
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/":
                state = "line"; i += 2; continue
            if c == "/" and n == "*":
                state = "block"; i += 2; continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return start, i + 1
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"; i += 2; continue
        elif state == "string":
            if c == "\\":
                i += 2; continue
            if c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 2; continue
            if c == "'":
                state = "code"
        i += 1
    raise RuntimeError(f"Unterminated function: {marker}")


def parent_map(tree: ast.AST):
    out = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            out[child] = parent
    return out


def find_statement_for_message(text: str, message: str):
    tree = ast.parse(text)
    parents = parent_map(tree)
    nodes = [
        node for node in ast.walk(tree)
        if isinstance(node, ast.Constant)
        and isinstance(node.value, str)
        and (node.value == message or node.value.endswith(message))
    ]
    if not nodes:
        return None
    node = nodes[0]
    stmt = None
    while node in parents:
        node = parents[node]
        if isinstance(node, ast.stmt):
            stmt = node
            break
    if stmt is None:
        return None
    parent = parents.get(stmt)
    if isinstance(parent, ast.If) and len(parent.body) == 1 and parent.body[0] is stmt:
        stmt = parent
    return stmt


def selfcheck_block(indent: str):
    return [
        indent + "# v16: correct pointer ABI + viewport-locked four-plane FOV/aspect guard.\n",
        indent + "_v16_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')\n",
        indent + "_v16 = _v16_path.read_text(encoding='utf-8-sig')\n",
        indent + "_v16_required = all(token in _v16 for token in (\n",
        indent + "    'position pointer',\n",
        indent + "    'position_address = static_cast<std::uint32_t>(context->r5)',\n",
        indent + "    'static_cast<std::uint32_t>(context->r6)',\n",
        indent + "    'selected_aspect(authored_aspect)',\n",
        indent + "    'effective_fov_radians(authored_fov_y)',\n",
        indent + "    'requested_horizontal_half',\n",
        indent + "    'requested_vertical_half',\n",
        indent + "    'target_unit_planes',\n",
        indent + "    'kTargetFrustumGuard = 1.20F',\n",
        indent + "    'v16 viewport-locked FOV/aspect guard active',\n",
        indent + "))\n",
        indent + "_v16_forbidden = any(token in _v16 for token in (\n",
        indent + "    'std::bit_cast<float>(static_cast<std::uint32_t>(context->r5))',\n",
        indent + "    'MEM_W(0x10, sp)',\n",
        indent + "    'kNoSideCullRadiusBits = 0x7F7FFFFFU',\n",
        indent + "    'target_diagonal_half',\n",
        indent + "))\n",
        indent + "if not (_v16_required and not _v16_forbidden):\n",
        indent + "    raise SystemExit('SOURCE SELF-CHECK FAILED: " + V16_MESSAGE.replace("'", "\\'") + "')\n",
    ]


def migrate_v15_block(text: str):
    marker = "# v15: real MIPS o32 frustum_test ABI + camera-forward-locked target frustum."
    pos = text.find(marker)
    if pos < 0:
        return None
    line_start = text.rfind("\n", 0, pos) + 1
    indent = re.match(r"[ \t]*", text[line_start:]).group(0)
    fail_pos = text.find(V15_MESSAGE, pos)
    if fail_pos < 0:
        return None
    end = text.find("\n", fail_pos)
    if end < 0:
        end = len(text)
    else:
        end += 1
    return text[:line_start] + "".join(selfcheck_block(indent)) + text[end:]


def migrate_self_check(text: str):
    if V16_MESSAGE in text or V16_MESSAGE.replace("'", "\\'") in text:
        return text
    migrated = migrate_v15_block(text)
    if migrated is not None:
        return migrated
    for message in OLD_MESSAGES:
        stmt = find_statement_for_message(text, message)
        if stmt is not None:
            lines = text.splitlines(keepends=True)
            start = stmt.lineno - 1
            end = stmt.end_lineno
            indent = re.match(r"[ \t]*", lines[start]).group(0)
            return "".join(lines[:start] + selfcheck_block(indent) + lines[end:])
    raise RuntimeError(
        "Known FIXED34/v12-v15 culling assertion not found in self_check.py; "
        "refusing to rewrite an unknown source-integrity check"
    )


def patch(root: Path):
    culling = root / "src" / "widescreen_culling.cpp"
    self_check = root / "scripts" / "self_check.py"
    if not culling.is_file() or not self_check.is_file():
        raise RuntimeError("Required Rocket-R culling/self-check files are missing")

    text, nl, bom = read_text(culling)
    if "#include <limits>" not in text:
        include_anchor = "#include <cstdint>\n"
        if include_anchor not in text:
            raise RuntimeError("Unable to add required <limits> include")
        text = text.replace(include_anchor, include_anchor + "#include <limits>\n", 1)

    start, end = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    text = text[:start] + CULLING_REPLACEMENT + text[end:]
    write_text(culling, text, nl, bom)

    sc, snl, sbom = read_text(self_check)
    write_text(self_check, migrate_self_check(sc), snl, sbom)

    final = culling.read_text(encoding="utf-8-sig")
    start, end = function_span(final, 'extern "C" void rocket_widescreen_frustum_begin')
    body = final[start:end]

    required = (
        "position_address = static_cast<std::uint32_t>(context->r5)",
        "static_cast<std::uint32_t>(context->r6)",
        "ReadVec3(rdram, position_ptr, 0)",
        "selected_aspect(authored_aspect)",
        "effective_fov_radians(authored_fov_y)",
        "requested_horizontal_half",
        "requested_vertical_half",
        "target_unit_planes",
        "kTargetFrustumGuard = 1.20F",
        "context->r6 = static_cast<gpr>",
        "v16 viewport-locked FOV/aspect guard active",
    )
    for token in required:
        if token not in body:
            raise RuntimeError("Missing v16 token: " + token)

    forbidden = (
        "std::bit_cast<float>(static_cast<std::uint32_t>(context->r5))",
        "MEM_W(0x10, sp)",
        "context->r7 =",
        "kNoSideCullRadiusBits = 0x7F7FFFFFU",
        "target_diagonal_half",
        "sphere_angle",
        "WriteVec3(rdram, camera",
    )
    for token in forbidden:
        if token in body:
            raise RuntimeError("Broken/retired culling token remains: " + token)

    print("[OK] Removed v15's incorrect inline-Vec3 ABI handling.")
    print("[OK] Restored Rocket Vec3f parameter decay: r5 is the position pointer.")
    print("[OK] Restored cullRadius in r6; renderDistance in r7 remains untouched.")
    print("[OK] Horizontal culling follows selected aspect AND effective vertical FOV.")
    print("[OK] Vertical culling follows effective vertical FOV directly.")
    print("[OK] Target planes rotate from Rocket's live camera-forward vector.")
    print("[OK] Exact rectangular sphere test replaces diagonal-cone approximation.")
    print("[OK] Shared camera side planes remain read-only; only r6 is relaxed per call.")
    print("[OK] Source self-check migrated to v16 pointer-ABI/FOV validation.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == "__main__":
    main()
