#!/usr/bin/env python3
"""Build a portable .nrm package without changing the game or its generated code."""
import argparse
import json
from pathlib import Path
import subprocess
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
import zipfile
import importlib.util


def project_file(project, relative):
    path = (project / relative).resolve()
    if not path.is_relative_to(project) or not path.is_file():
        raise ValueError(f"Missing file or file outside the project: {relative}")
    return path


def package_files(project, manifest):
    """Explicit resources preserve their relative names in the final package."""
    files = {}
    for name, relative in manifest.get("files", {}).items():
        if (not name or name.startswith("/") or "\\" in name or ":" in name
                or any(part in ("", ".", "..") for part in name.split("/"))
                or any(ord(c) < 32 for c in name)):
            raise ValueError(f"Unsafe package filename: {name}")
        if name.lower() in ("mod.json", "mod_binary.bin", "mod_syms.bin", "rocket.json", "thumb.png"):
            raise ValueError(f"Reserved package filename: {name}")
        if Path(name).suffix.lower() in (".dll", ".so", ".exe", ".dylib"):
            raise ValueError("Native executables are not accepted in portable mods")
        if name.lower() in {key.lower() for key in files}:
            raise ValueError(f"Case-insensitive package filename collision: {name}")
        files[name] = project_file(project, relative)
    return files


def project_sources(project, manifest):
    patterns = manifest.get("build", {}).get("sources", ["*.c"])
    if not isinstance(patterns, list) or not all(isinstance(p, str) for p in patterns):
        raise ValueError("build.sources must be an array of project-relative patterns")
    sources = set()
    for pattern in patterns:
        if Path(pattern).is_absolute() or ".." in Path(pattern).parts:
            raise ValueError("Source patterns must remain inside the project")
        for path in project.glob(pattern):
            if path.is_file() and path.suffix == ".c":
                sources.add(project_file(project, path.relative_to(project)))
    return sorted(sources)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--linker", default="ld.lld")
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--data-symbols", type=Path, action="append", default=[],
                        help="Matching game data symbols; repeat for additional reference files")
    parser.add_argument("--output", type=Path, default=Path("build/mods"))
    parser.add_argument("--wsl", action="store_true", help="Use Clang and LLD in Ubuntu-24.04 on Windows")
    parser.add_argument("--wsl-distro", default="Ubuntu-24.04")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    project = args.project.resolve()
    output = args.output.resolve()
    source = project / "mod.toml"
    manifest = tomllib.loads(source.read_text(encoding="utf-8"))
    resources = package_files(project, manifest)
    asset_paths = package_files(project, {"files": {name: recipe["source"] for name, recipe in manifest.get("assets", {}).items()}})
    asset_bytes = {}
    if asset_paths:
        spec = importlib.util.spec_from_file_location("rocket_sdk_assets",Path(__file__).with_name("rocket_sdk.py"))
        sdk_assets = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sdk_assets)
        for name, path in asset_paths.items():
            recipe = manifest["assets"][name]
            if name.lower() in {n.lower() for n in resources}:
                raise ValueError(f"Asset/resource collision: {name}")
            if recipe.get("format")=="obj":
                texture_paths=package_files(project,{"files":{name:recipe["texture"]}}) if recipe.get("texture") else {}
                asset_bytes[name] = sdk_assets.compile_mesh(path,recipe.get("colour","FFFFFFFF"),recipe.get("y_up",False),texture_paths.get(name))
            elif recipe.get("format")=="tones":
                asset_bytes[name]=sdk_assets.compile_tones(path)
            else:
                raise ValueError("Asset format must be obj or tones")
    mod_id = manifest["manifest"]["id"]
    work = output / mod_id
    work.mkdir(parents=True, exist_ok=True)
    def compile_command(command):
        if args.wsl:
            def linux_path(value):
                if len(value) > 2 and value[1] == ":":
                    return "/mnt/" + value[0].lower() + value[2:].replace("\\", "/")
                return value
            command = ["wsl", "-d", args.wsl_distro, "--", *map(linux_path, command)]
        subprocess.run(command, check=True)
    objects = []
    sources = project_sources(project, manifest)
    metadata = json.loads((project / "rocket.json").read_text(encoding="utf-8")) if (project / "rocket.json").is_file() else {}
    if metadata.get("api", 1) == 2:
        sources.append(root / "modding/lib/memory.c")
    for c in sources:
        obj = work / (c.relative_to(project).with_suffix(".o") if c.is_relative_to(project) else Path("sdk/memory.o"))
        obj.parent.mkdir(parents=True, exist_ok=True)
        compile_command([args.clang, "-target", "mips", "-mips2", "-mabi=32", "-O2", "-G0",
            "-mno-abicalls", "-mno-odd-spreg", "-mno-check-zero-division", "-fomit-frame-pointer",
            "-fno-builtin", "-ffreestanding", "-nostdinc", "-ffunction-sections", "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "modding/include"), "-c", str(c), "-o", str(obj)])
        objects.append(str(obj))
    if not objects:
        parser.error("Project has no .c files")
    elf = work / "mod.elf"
    compile_command([args.linker, "-nostdlib", "-T", str(root / "modding/mod.ld"),
        "--unresolved-symbols=ignore-all", "--emit-relocs", "-e", "0", "--no-nmagic", "--gc-sections",
        "-o", str(elf), *objects])
    additional = [p.resolve().as_posix() for p in (project / "rocket.json", project / "thumb.png") if p.is_file()]
    config = source.read_text(encoding="utf-8") + "\n[inputs]\n" + "\n".join(
        key + " = " + json.dumps(value) for key, value in {
            "elf_path": elf.as_posix(), "mod_filename": mod_id,
            "func_reference_syms_file": args.symbols.resolve().as_posix(),
            "data_reference_syms_files": [p.resolve().as_posix() for p in args.data_symbols], "additional_files": additional}.items()) + "\n"
    config_path = work / "build.toml"
    config_path.write_text(config, encoding="utf-8")
    subprocess.run([str(args.tool.resolve()), str(config_path), str(work)], check=True)
    # Stable ZIP metadata makes profile hashes independent of build time.
    with zipfile.ZipFile(work / (mod_id + ".nrm")) as source_zip:
        with zipfile.ZipFile(output / (mod_id + ".nrm"), "w", compression=zipfile.ZIP_DEFLATED) as destination:
            contents = {name: source_zip.read(name) for name in source_zip.namelist()}
            for name, path in resources.items():
                if name.lower() in {key.lower() for key in contents}:
                    raise ValueError(f"Package file collision: {name}")
                contents[name] = path.read_bytes()
            for name,data in asset_bytes.items():
                if name.lower() in {key.lower() for key in contents}:
                    raise ValueError(f"Package asset collision: {name}")
                contents[name] = data
            for name in sorted(contents):
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                destination.writestr(info, contents[name])
    if mod_id == "rocket_modern_camera":
        payload = (output / (mod_id + ".nrm")).read_bytes()
        header = root / "generated/mod_camera.generated.hpp"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("// Generated by scripts/build_mod.py.\n#pragma once\n#include <cstdint>\n"
            "namespace rocket::generated {\ninline constexpr std::uint8_t kCameraMod[] = {\n" +
            "\n".join(",".join(str(b) for b in payload[i:i+32])+"," for i in range(0,len(payload),32)) +
            "\n};\n}\n", encoding="utf-8")
    print(output / (mod_id + ".nrm"))

if __name__ == "__main__":
    main()
