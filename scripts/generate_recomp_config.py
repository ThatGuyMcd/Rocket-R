#!/usr/bin/env python3
"""Create N64Recomp TOML from Rocket-R's checked patch policy."""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def hook_statement(text: str) -> str:
    # Hooks may be emitted immediately after a generated branch label. A
    # declaration is not a statement in C17 (Android's Clang rejects it).
    # A compound statement is legal there and scopes each hook's temporaries.
    return "{ " + text + " }"


def policy_integer(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("policy addresses/instructions must be integer strings or integers")
    number = int(value, 0) if isinstance(value, str) else int(value)
    if not 0 <= number <= 0xFFFFFFFF:
        raise ValueError(f"invalid 32-bit policy integer: {value!r}")
    return number


def validate_policy(policy: dict, rom: bytes | None = None) -> None:
    if policy.get("schemaVersion") != 1:
        raise ValueError("unsupported Rocket-R policy schema")
    for collection, address_field in (("functionHooks", "beforeVram"),
                                      ("instructionPatches", "vram")):
        seen = set()
        for entry in policy.get(collection, []):
            address = policy_integer(entry[address_field])
            key = (entry["function"], address)
            if address % 4 or key in seen:
                raise ValueError(f"duplicate or unaligned {collection} entry: {key}")
            seen.add(key)
            if rom is not None and "expectedInstruction" in entry:
                offset = policy_integer(entry["romOffset"])
                if int.from_bytes(rom[offset:offset+4], "big") != policy_integer(entry["expectedInstruction"]):
                    raise ValueError(f"policy instruction {key} does not match the supplied ROM")
            if "callsiteTarget" in entry:
                target = policy_integer(entry["callsiteTarget"])
                instruction = policy_integer(entry["expectedInstruction"])
                offset = policy_integer(entry["romOffset"])
                decoded = ((address + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
                if instruction >> 26 != 3 or decoded != target:
                    raise ValueError(f"callsite {key} is not a JAL to its declared target")
                if rom is not None and (offset + 4 > len(rom) or
                        int.from_bytes(rom[offset:offset + 4], "big") != instruction):
                    raise ValueError(f"callsite {key} does not match the supplied ROM")


def string_list(entries: list[object]) -> str:
    values: list[str] = []
    for entry in entries:
        if isinstance(entry, str):
            values.append(entry)
        elif isinstance(entry, dict) and "name" in entry:
            values.append(str(entry["name"]))
        else:
            raise ValueError(f"invalid policy string-list entry: {entry!r}")
    return ", ".join(toml_string(value) for value in values)


def write_mod_protection(policy: dict, elf_path: Path) -> None:
    """Protect every function changed by the checked policy from raw-ROM regeneration."""
    data = elf_path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x02":
        raise ValueError("Expected the big-endian ELF32 Rocket build")
    header = struct.unpack_from(">HHIIIIIHHHHHH", data, 16)
    sections = [struct.unpack_from(">10I", data, header[5] + i * header[10]) for i in range(header[11])]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        for pos in range(section[4], section[4] + section[5], section[9]):
            name, value = struct.unpack_from(">II", data, pos)
            start = strings[4] + name
            name = data[start:data.index(b"\0", start)].decode("ascii")
            symbols[name] = value
    names = {e["function"] for key in ("functionHooks", "instructionPatches") for e in policy.get(key, [])}
    names.update(e if isinstance(e, str) else e["name"] for e in policy.get("stubs", []))
    missing = names - symbols.keys()
    if missing:
        raise ValueError(f"Missing protected function symbols: {sorted(missing)}")
    output = Path(__file__).resolve().parents[1] / "generated/mod_protection.generated.hpp"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("// Generated from the checked recomp policy.\n#pragma once\n#include <cstdint>\n"
        "namespace rocket::generated {\ninline constexpr std::uint32_t kModProtectedFunctions[] = {\n" +
        "".join(f"    0x{symbols[n]:08X}U, // {n}\n" for n in sorted(names)) + "};\n}\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--output-functions", required=True, type=Path)
    parser.add_argument("--entrypoint", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--functions-per-output-file", type=int, default=50)
    args = parser.parse_args()

    policy = json.loads(args.policy.read_text(encoding="utf-8"))
    validate_policy(policy, args.rom.read_bytes())
    write_mod_protection(policy, args.elf)

    manual = ", ".join(
        "{ name = %s, section = %s, vram = %s, size = %s }" % (
            toml_string(str(e["name"])), toml_string(str(e["section"])), e["vram"], e["size"])
        for e in policy.get("manualFunctions", []))
    sizes = ", ".join(
        "{ name = %s, size = %s }" % (toml_string(str(e["name"])), e["size"])
        for e in policy.get("functionSizes", []))
    instruction_patches = "\n\n".join(
        "[[patches.instruction]]\nfunc = %s\nvram = %s\nvalue = %s" % (
            toml_string(str(e["function"])), e["vram"], e["value"])
        for e in policy.get("instructionPatches", []))
    hooks = "\n\n".join(
        "[[patches.hook]]\nfunc = %s\nbefore_vram = %s\ntext = %s" % (
            toml_string(str(e["function"])), e["beforeVram"], toml_string(hook_statement(str(e["text"]))))
        for e in policy.get("functionHooks", []))

    content = f'''# Generated by scripts/generate_recomp_config.py. Do not edit by hand.
[input]
entrypoint = {args.entrypoint}
use_mdebug = false
elf_path = {toml_string(args.elf.resolve().as_posix())}
rom_file_path = {toml_string(args.rom.resolve().as_posix())}
output_func_path = {toml_string(args.output_functions.resolve().as_posix())}
functions_per_output_file = {args.functions_per_output_file}
manual_funcs = [{manual}]
function_sizes = [{sizes}]

[patches]
stubs = [{string_list(policy.get('stubs', []))}]
renamed = [{string_list(policy.get('renamed', []))}]
ignored = [{string_list(policy.get('ignored', []))}]

{instruction_patches}

{hooks}
'''
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8", newline="\n")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
