#!/usr/bin/env python3
from pathlib import Path
import argparse
import ast
import re


CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    if (!g_expand_enabled.load(std::memory_order_acquire)) return;

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

    // Never narrow retail visibility. We only add the presentation area that
    // becomes visible because of a wider aspect ratio and/or positive FOV.
    const float target_aspect = std::max(authored_aspect, selected_aspect);
    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);
    const bool aspect_expanded =
        target_aspect > authored_aspect * 1.0001F;
    const bool fov_expanded =
        target_fov_y > authored_fov_y + 0.0001F;
    if (!aspect_expanded && !fov_expanded) return;

    std::array<Vec3, 4> authored_planes{};
    std::array<Vec3, 4> unit_planes{};
    std::array<Vec3, 4> target_planes{};
    std::array<float, 4> plane_lengths{};

    Vec3 outward_sum{0.0F, 0.0F, 0.0F};
    for (std::size_t index = 0; index < authored_planes.size(); ++index) {
        authored_planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Finite(authored_planes[index])) return;
        if (!Normalize(authored_planes[index],
                       unit_planes[index],
                       &plane_lengths[index])) {
            return;
        }
        outward_sum = Add(outward_sum, unit_planes[index]);
    }

    // For a perspective frustum all four outward side-plane normals share a
    // component pointing back toward the camera. Their negative sum therefore
    // gives the frustum centre/forward axis. This is derived from the exact
    // planes Rocket is using for this call, so it is stable under arbitrary
    // camera rotations and avoids the guessed camera-basis offset used by v12.
    Vec3 forward{};
    if (!Normalize(Scale(outward_sum, -1.0F), forward)) return;

    float forward_score = 0.0F;
    for (const Vec3& plane : unit_planes) {
        forward_score += Dot(plane, forward);
    }
    if (forward_score > 0.0F) {
        forward = Scale(forward, -1.0F);
    }

    // Find Rocket's two opposite plane pairs without relying on memory order.
    // Of the three possible pairings, the real left/right + top/bottom pairing
    // has the most opposing normals (smallest summed pair dot product).
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
            -0.5F * (Dot(unit_planes[pair[0]], forward) +
                     Dot(unit_planes[pair[1]], forward)),
            0.0F, 1.0F);
        authored_half_fov[pair_index] = std::asin(forward_component);
        if (!std::isfinite(authored_half_fov[pair_index])) return;
    }

    // Rocket's authored aspect is wider than 1:1, so the pair with the larger
    // authored half-angle is the horizontal left/right pair.
    const std::size_t horizontal_pair =
        authored_half_fov[0] >= authored_half_fov[1] ? 0U : 1U;

    // Add a guarded margin in tangent space. This expands naturally with FOV
    // and keeps objects stable at the exact visible edge without globally
    // widening Rocket's stored camera planes.
    constexpr float kTargetFrustumGuard = 1.15F;
    float vertical_tangent = std::tan(target_fov_y * 0.5F);
    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;
    vertical_tangent *= kTargetFrustumGuard;

    const float requested_vertical_half =
        std::atan(vertical_tangent);
    const float requested_horizontal_half =
        std::atan(vertical_tangent * target_aspect);
    if (!std::isfinite(requested_vertical_half) ||
        !std::isfinite(requested_horizontal_half)) {
        return;
    }

    // Build the exact target left/right/top/bottom planes from the authored
    // planes themselves. Each plane keeps Rocket's original magnitude and
    // transverse sign; only its half-FOV angle is widened.
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

            const Vec3 target_unit = Add(
                Scale(transverse, std::cos(target_half)),
                Scale(forward, -std::sin(target_half)));
            target_planes[index] = Scale(target_unit, plane_lengths[index]);
        }
    }

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

    // A small distance-scaled edge slack prevents one-frame oscillation caused
    // by presentation interpolation/float quantisation exactly on a plane.
    const float edge_slack = std::max(0.75F, distance * 0.006F);

    // Exact target-frustum sphere test. If Rocket's object sphere is outside
    // even the widened target rectangle, leave retail culling untouched.
    for (const Vec3& target_plane : target_planes) {
        const float plane_distance = Dot(camera_offset, target_plane);
        if (!std::isfinite(plane_distance)) return;
        if (plane_distance > authored_radius + edge_slack) {
            return;
        }
    }

    // The object is genuinely inside the visible widened rectangle. Relax only
    // this call's cullRadius enough for Rocket's original four authored plane
    // tests to accept it. Distance culling remains completely untouched.
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
            "[culling] exact object-local target frustum active; "
            "plane-derived forward/pairs, no shared-plane rewrite "
            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, 15%% guard)\n",
            authored_aspect, target_aspect,
            authored_fov_y * (180.0F / kPi),
            target_fov_y * (180.0F / kPi));
    }
}'''


V12_MESSAGE = (
    "FIXED34/v12 must use object-local FOV/aspect culling, preserve the "
    "window-aspect bridge, and avoid rewriting shared camera frustum planes"
)
OLD_MESSAGE = (
    "FIXED34 must widen only Rocket's horizontal CPU frustum in Expand mode "
    "with a guarded, idempotent window-aspect bridge"
)
V13_MESSAGE = (
    "FIXED34/v13.1 must use exact object-local target-frustum culling derived "
    "from Rocket's authored four side planes"
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


def replace_function(text: str, marker: str, replacement: str):
    start, end = function_span(text, marker)
    return text[:start] + replacement + text[end:]


def parent_map(tree: ast.AST):
    result = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            result[child] = parent
    return result


def assertion_statement_for_message(text: str, message: str):
    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        raise RuntimeError(f"Unable to parse self_check.py: {exc}") from exc
    parents = parent_map(tree)
    matches = [
        node for node in ast.walk(tree)
        if isinstance(node, ast.Constant)
        and isinstance(node.value, str)
        and (node.value == message or node.value.endswith(message))
    ]
    if len(matches) != 1:
        return None
    node = matches[0]
    stmt = None
    while node in parents:
        node = parents[node]
        if isinstance(node, ast.stmt):
            stmt = node
            break
    if stmt is not None:
        parent = parents.get(stmt)
        if isinstance(parent, ast.If) and len(parent.body) == 1 and parent.body[0] is stmt:
            stmt = parent
    return stmt


def migrate_self_check(text: str):
    if "FIXED34/v13.1 must use exact object-local target-frustum culling" in text:
        return text

    source_message = V12_MESSAGE if V12_MESSAGE in text else OLD_MESSAGE if OLD_MESSAGE in text else None
    if source_message is None:
        raise RuntimeError(
            "Known FIXED34/v12 culling assertion was not found in self_check.py; "
            "refusing to rewrite an unknown integrity check"
        )

    stmt = assertion_statement_for_message(text, source_message)
    if stmt is None or not hasattr(stmt, "lineno") or not hasattr(stmt, "end_lineno"):
        raise RuntimeError("Could not isolate the existing culling assertion in self_check.py")

    lines = text.splitlines(keepends=True)
    start = stmt.lineno - 1
    end = stmt.end_lineno
    indent = re.match(r"[ \t]*", lines[start]).group(0)

    replacement = [
        indent + "# v13.1: exact object-local target-frustum culling; global integrity check is scoped to genuine retired cone markers.\n",
        indent + "_v13_culling_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')\n",
        indent + "_v13_culling = _v13_culling_path.read_text(encoding='utf-8-sig')\n",
        indent + "_v13_required = all(token in _v13_culling for token in (\n",
        indent + "    'kPairings',\n",
        indent + "    'outward_sum',\n",
        indent + "    'target_planes',\n",
        indent + "    'requested_horizontal_half',\n",
        indent + "    'requested_vertical_half',\n",
        indent + "    'kTargetFrustumGuard = 1.15F',\n",
        indent + "    'plane_distance > authored_radius + edge_slack',\n",
        indent + "    'required_radius = std::max(required_radius, plane_distance)',\n",
        indent + "    'context->r6 = static_cast<gpr>',\n",
        indent + "))\n",
        indent + "_v13_forbidden = any(token in _v13_culling for token in (\n",
        indent + "    'target_diagonal_half',\n",
        indent + "    'sphere_angle',\n",
        indent + "    'kForwardOffset), forward',\n",
        indent + "))\n",
        indent + "if not (_v13_required and not _v13_forbidden):\n",
        indent + "    raise SystemExit('SOURCE SELF-CHECK FAILED: " + V13_MESSAGE.replace("'", "\\'") + "')\n",
    ]
    return "".join(lines[:start] + replacement + lines[end:])


def patch_known_verifier(text: str):
    text = text.replace("'target_diagonal_half'", "'target_planes'")
    text = text.replace("'sphere_angle'", "'kPairings'")
    text = text.replace("'kObjectCullGuard'", "'kTargetFrustumGuard'")
    text = text.replace("'kAngularHysteresis'", "'edge_slack'")
    text = text.replace(
        "v12 object-local culling guard missing token",
        "v13 exact target-frustum culling guard missing token")
    text = text.replace(
        "v12 culling must not rewrite shared camera frustum planes",
        "v13 culling must not rewrite shared camera frustum planes")
    text = text.replace(
        "Rocket-R Graphics v12 stable culling + UI cleanup verification PASS.",
        "Rocket-R Graphics v12/v13 stable exact-frustum culling + UI cleanup verification PASS.")
    text = text.replace(
        "Rocket-R Graphics v10/v12 view-distance + object-local FOV culling verification PASS.",
        "Rocket-R Graphics v10/v13 view-distance + exact object-local FOV culling verification PASS.")
    return text


def patch(root: Path):
    culling = root / "src" / "widescreen_culling.cpp"
    self_check = root / "scripts" / "self_check.py"

    if not culling.is_file():
        raise RuntimeError(f"Required source file not found: {culling}")
    if not self_check.is_file():
        raise RuntimeError(f"Required source-integrity file not found: {self_check}")

    text, nl, bom = read_text(culling)
    text = replace_function(
        text,
        'extern "C" void rocket_widescreen_frustum_begin',
        CULLING_REPLACEMENT)
    write_text(culling, text, nl, bom)

    sc, sc_nl, sc_bom = read_text(self_check)
    write_text(self_check, migrate_self_check(sc), sc_nl, sc_bom)

    for rel in [
        "scripts/verify_graphics_v12.py",
        "scripts/verify_graphics_v11.py",
        "scripts/verify_view_distance_culling_v10.py",
    ]:
        path = root / rel
        if path.is_file():
            vt, vnl, vbom = read_text(path)
            write_text(path, patch_known_verifier(vt), vnl, vbom)

    final = culling.read_text(encoding="utf-8-sig")
    start, end = function_span(
        final, 'extern "C" void rocket_widescreen_frustum_begin')
    body = final[start:end]

    required = [
        "kPairings",
        "outward_sum",
        "target_planes",
        "requested_horizontal_half",
        "requested_vertical_half",
        "kTargetFrustumGuard = 1.15F",
        "plane_distance > authored_radius + edge_slack",
        "required_radius = std::max(required_radius, plane_distance)",
        "context->r6 = static_cast<gpr>",
    ]
    for token in required:
        if token not in body:
            raise RuntimeError(f"v13.1 culling token missing after patch: {token}")

    for token in [
        "target_diagonal_half",
        "sphere_angle",
        "kForwardOffset), forward",
        "WriteVec3(rdram, camera",
    ]:
        if token in body:
            raise RuntimeError(f"Retired v12 culling path remains in v13.1: {token}")

    print("[OK] Removed v12 diagonal-cone culling approximation.")
    print("[OK] Forward axis now derives from Rocket's four authored side planes.")
    print("[OK] Left/right and top/bottom pairs are detected from plane geometry, not memory order.")
    print("[OK] Exact widened rectangular target frustum installed with 15% tangent-space guard.")
    print("[OK] Only target-visible objects receive per-call cullRadius relaxation; distance culling is unchanged.")
    print("[OK] FIXED34/v12 self-check migrated to corrected v13.1 target-frustum validation.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == "__main__":
    main()
