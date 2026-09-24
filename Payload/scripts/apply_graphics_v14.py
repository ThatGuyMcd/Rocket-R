#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, re

CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (context == nullptr) return;
    if (!g_expand_enabled.load(std::memory_order_acquire)) return;

    // Expanded RT64 presentation must never be clipped by Rocket's original
    // CPU side planes. renderDistance is a separate argument (r7), so this
    // changes only side-plane rejection and leaves distance culling/fade alone.
    const rocket::graphics::Settings settings = rocket::graphics::settings();
    const bool expanded_presentation =
        rocket::graphics::widescreen_active(4.0F / 3.0F) ||
        settings.fov_offset_degrees > 0.001F;
    if (!expanded_presentation) return;

    // IEEE-754 FLT_MAX. frustum_test's side-plane code is:
    //   if (cullRadius < dot(cameraOffset, sidePlane)) return 2;
    // A FLT_MAX cullRadius therefore neutralizes only that old CPU rejection.
    constexpr std::uint32_t kNoSideCullRadiusBits = 0x7F7FFFFFU;
    context->r6 = static_cast<gpr>(kNoSideCullRadiusBits);

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(
            stderr,
            "[culling] expanded presentation: Rocket CPU side-plane rejection "
            "bypassed; RT64/GPU viewport clipping owns visibility; "
            "culling/fade remains active\n");
    }

    (void)rdram;
}'''

OLD_MESSAGES = (
    "FIXED34 must widen only Rocket's horizontal CPU frustum in Expand mode with a guarded, idempotent window-aspect bridge",
    "FIXED34/v12 must use object-local FOV/aspect culling, preserve the window-aspect bridge, and avoid rewriting shared camera frustum planes",
    "FIXED34/v13 must use exact object-local target-frustum culling derived from Rocket's authored four side planes",
    "FIXED34/v13.1 must use exact object-local target-frustum culling derived from Rocket's authored four side planes",
)
V14_MESSAGE = "FIXED34/v14 must bypass Rocket CPU side-plane rejection in expanded presentation modes while preserving render-distance culling"

def read_text(path):
    raw = path.read_bytes(); bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    return text, ("\r\n" if "\r\n" in text else "\n"), bom

def write_text(path, text, nl, bom):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n": text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom: data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)

def function_span(text, marker):
    start = text.find(marker)
    if start < 0: raise RuntimeError(f"Function marker not found: {marker}")
    brace = text.find("{", start)
    depth = 0; state = "code"; i = brace
    while i < len(text):
        c = text[i]; n = text[i+1] if i+1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/": state = "line"; i += 2; continue
            if c == "/" and n == "*": state = "block"; i += 2; continue
            if c == '"': state = "string"
            elif c == "'": state = "char"
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0: return start, i+1
        elif state == "line":
            if c == "\n": state = "code"
        elif state == "block":
            if c == "*" and n == "/": state = "code"; i += 2; continue
        elif state == "string":
            if c == "\\": i += 2; continue
            if c == '"': state = "code"
        elif state == "char":
            if c == "\\": i += 2; continue
            if c == "'": state = "code"
        i += 1
    raise RuntimeError(f"Unterminated function: {marker}")

def parent_map(tree):
    out = {}
    for p in ast.walk(tree):
        for c in ast.iter_child_nodes(p): out[c] = p
    return out

def find_statement_for_message(text, message):
    tree = ast.parse(text); parents = parent_map(tree)
    nodes = [n for n in ast.walk(tree)
             if isinstance(n, ast.Constant) and isinstance(n.value, str)
             and (n.value == message or n.value.endswith(message))]
    if not nodes: return None
    node = nodes[0]; stmt = None
    while node in parents:
        node = parents[node]
        if isinstance(node, ast.stmt):
            stmt = node; break
    if stmt is None: return None
    p = parents.get(stmt)
    if isinstance(p, ast.If) and len(p.body) == 1 and p.body[0] is stmt: stmt = p
    return stmt

def selfcheck_block(indent):
    return [
        indent + "# v14: expanded presentation bypasses Rocket's stale CPU side-plane rejection.\n",
        indent + "_v14_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')\n",
        indent + "_v14 = _v14_path.read_text(encoding='utf-8-sig')\n",
        indent + "_v14_required = all(token in _v14 for token in (\n",
        indent + "    'kNoSideCullRadiusBits = 0x7F7FFFFFU',\n",
        indent + "    'rocket::graphics::widescreen_active(4.0F / 3.0F)',\n",
        indent + "    'settings.fov_offset_degrees > 0.001F',\n",
        indent + "    'context->r6 = static_cast<gpr>(kNoSideCullRadiusBits)',\n",
        indent + "    'culling/fade remains active',\n",
        indent + "))\n",
        indent + "_v14_forbidden = any(token in _v14 for token in (\n",
        indent + "    'target_diagonal_half', 'target_planes',\n",
        indent + "    'requested_horizontal_half', 'requested_vertical_half',\n",
        indent + "    'required_radius = std::max(required_radius, plane_distance)',\n",
        indent + "))\n",
        indent + "if not (_v14_required and not _v14_forbidden):\n",
        indent + "    raise SystemExit('SOURCE SELF-CHECK FAILED: " + V14_MESSAGE + "')\n",
    ]

def migrate_self_check(text):
    if V14_MESSAGE in text: return text
    for msg in reversed(OLD_MESSAGES):
        stmt = find_statement_for_message(text, msg)
        if stmt is not None:
            lines = text.splitlines(keepends=True)
            start, end = stmt.lineno-1, stmt.end_lineno
            indent = re.match(r"[ \t]*", lines[start]).group(0)
            return "".join(lines[:start] + selfcheck_block(indent) + lines[end:])
    raise RuntimeError("Known FIXED34/v12/v13 culling assertion not found in self_check.py")

def patch(root):
    culling = root/"src/widescreen_culling.cpp"
    self_check = root/"scripts/self_check.py"
    if not culling.is_file() or not self_check.is_file():
        raise RuntimeError("Required Rocket-R culling/self-check files are missing")

    text,nl,bom = read_text(culling)
    s,e = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    text = text[:s] + CULLING_REPLACEMENT + text[e:]
    write_text(culling, text, nl, bom)

    sc,snl,sbom = read_text(self_check)
    write_text(self_check, migrate_self_check(sc), snl, sbom)

    final = culling.read_text(encoding="utf-8-sig")
    s,e = function_span(final, 'extern "C" void rocket_widescreen_frustum_begin')
    body = final[s:e]
    for token in (
        "kNoSideCullRadiusBits = 0x7F7FFFFFU",
        "rocket::graphics::widescreen_active(4.0F / 3.0F)",
        "settings.fov_offset_degrees > 0.001F",
        "context->r6 = static_cast<gpr>(kNoSideCullRadiusBits)",
        "culling/fade remains active",
    ):
        if token not in body: raise RuntimeError("Missing v14 token: " + token)
    for token in (
        "target_diagonal_half", "target_planes",
        "requested_horizontal_half", "requested_vertical_half",
        "required_radius = std::max(required_radius, plane_distance)",
        "ReadVec3(rdram, camera", "WriteVec3(rdram, camera",
    ):
        if token in body: raise RuntimeError("Retired culling path remains: " + token)

    print("[OK] Removed camera-derived v12/v13 frustum approximation.")
    print("[OK] Expanded aspect/FOV modes now bypass Rocket CPU side-plane rejection.")
    print("[OK] renderDistance/r7 remains untouched; Draw Distance and distance fade remain active.")
    print("[OK] Original 4:3 with no positive FOV offset keeps retail side-plane culling.")
    print("[OK] Source self-check migrated to v14 viewport-safe culling.")

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--root", required=True)
    patch(Path(ap.parse_args().root).resolve())

if __name__ == "__main__": main()
