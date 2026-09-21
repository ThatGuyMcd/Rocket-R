#!/usr/bin/env python3
"""Prepare immutable external dependencies at the commits in dependencies.lock.json.

FIXED19 deliberately prefers already-present pinned Git objects. A successful
Rocket-R build should not become dependent on a fresh GitHub fetch every time
it is rebuilt. Network access is used only when a required commit is genuinely
missing from the local checkout.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def git_args(*args: str) -> list[str]:
    # Dependencies are built by both Windows and WSL. Force LF checkouts locally
    # so a user's global core.autocrlf setting cannot turn Linux shebangs into
    # /usr/bin/env: 'python3\r' or 'bash\r' failures.
    return ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", *args]


def run(args: list[str], cwd: Path | None = None, capture: bool = False) -> str:
    printable = " ".join(str(x) for x in args)
    print("+", printable, flush=True)
    result = subprocess.run(
        args,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    if result.returncode != 0:
        if capture and result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", file=sys.stderr)
        raise RuntimeError(f"command failed with exit code {result.returncode}: {printable}")
    return (result.stdout or "").strip()


def run_checked(args: list[str], cwd: Path, description: str) -> str:
    """Run a command with merged captured output so failures remain diagnostic."""
    printable = " ".join(str(x) for x in args)
    print("+", printable, flush=True)
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout or ""
    if output:
        print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if result.returncode != 0:
        raise RuntimeError(f"{description} failed with exit code {result.returncode}: {printable}")
    return output.strip()


def has_commit(repository: Path, commit: str) -> bool:
    if not repository.exists():
        return False

    # The common rebuild path is already detached at the exact pinned commit.
    # Recognise that without consulting a remote or relying on cat-file's
    # revision-expression handling on Git-for-Windows.
    head = subprocess.run(
        git_args("rev-parse", "HEAD"),
        cwd=repository,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if head.returncode == 0 and (head.stdout or "").strip().lower() == commit.lower():
        return True

    probe = subprocess.run(
        git_args("rev-parse", "--verify", f"{commit}^{{commit}}"),
        cwd=repository,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return probe.returncode == 0


def ensure_git_checkout(root: Path, destination: Path, repository: str) -> None:
    if destination.exists() and not (destination / ".git").exists():
        try:
            nonempty = any(destination.iterdir())
        except OSError as exc:
            raise RuntimeError(f"Could not inspect dependency directory {destination}: {exc}") from exc
        if nonempty:
            raise RuntimeError(
                f"{destination} exists but is not a Git checkout. Move/remove that directory and rerun."
            )
        destination.rmdir()

    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        # Clone metadata first, then checkout the exact pinned object. Recursive
        # submodules are populated after the parent commit is selected.
        run(git_args("clone", "--filter=blob:none", "--no-checkout", repository, str(destination)), cwd=root)


def checkout_dependency(root: Path, dep: dict[str, object], repair: bool) -> None:
    destination = root / str(dep["destination"])
    repository = str(dep["repository"])
    commit = str(dep["commit"])
    recursive = bool(dep.get("recursive", False))

    ensure_git_checkout(root, destination, repository)

    # extern/ is generated. Discard prior patches/interrupted builds before
    # selecting the pinned object. This intentionally preserves build/private,
    # because it lives outside extern/.
    run(git_args("config", "core.autocrlf", "false"), cwd=destination)
    run(git_args("config", "core.eol", "lf"), cwd=destination)
    run(git_args("reset", "--hard"), cwd=destination)
    run(git_args("clean", "-ffd"), cwd=destination)

    if has_commit(destination, commit):
        print(f"= using locally cached pinned commit for {dep['name']}: {commit}", flush=True)
    else:
        print(f"= pinned commit is not cached for {dep['name']}; fetching only {commit}", flush=True)
        run_checked(
            git_args("fetch", "--no-tags", "--force", "origin", commit),
            destination,
            f"Fetching pinned commit for {dep['name']}",
        )

    run(git_args("checkout", "--detach", commit), cwd=destination)
    run(git_args("reset", "--hard", commit), cwd=destination)
    run(git_args("clean", "-ffd"), cwd=destination)

    if recursive:
        run(git_args("submodule", "sync", "--recursive"), cwd=destination)
        # --force is intentional: N64Recomp is patched by Rocket-R and a later
        # one-click run must be able to restore the parent dependency without a
        # dirty submodule blocking checkout.
        run_checked(
            git_args("submodule", "update", "--init", "--recursive", "--force"),
            destination,
            f"Populating submodules for {dep['name']}",
        )

    actual = run(git_args("rev-parse", "HEAD"), cwd=destination, capture=True)
    if actual.lower() != commit.lower():
        raise RuntimeError(f"{dep['name']} is at {actual}, expected {commit}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def apply_patches(root: Path) -> None:
    manifest_path = root / "patches" / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 1:
        raise RuntimeError("unsupported patch manifest schema")

    for dependency in manifest.get("dependencies", []):
        repo = root / dependency["repositoryPath"]
        expected = str(dependency["expectedCommit"])
        actual = run(git_args("rev-parse", "HEAD"), cwd=repo, capture=True)
        if actual.lower() != expected.lower():
            raise RuntimeError(f"Patch target {repo} is {actual}, expected {expected}")

        # bootstrap always restores dependencies to their pinned commit first,
        # so patch application should always begin from a clean tracked tree.
        run(git_args("reset", "--hard", expected), cwd=repo)
        run(git_args("clean", "-ffd"), cwd=repo)

        print(f"Applying {len(dependency.get('patches', []))} patch(es) to {dependency['name']}...", flush=True)
        for patch in dependency.get("patches", []):
            patch_path = root / patch["path"]
            if not patch_path.is_file():
                raise RuntimeError(f"Missing patch file: {patch_path}")
            expected_hash = str(patch.get("sha256", "")).lower()
            if not expected_hash:
                raise RuntimeError(f"Patch has no SHA-256 in manifest: {patch_path}")
            actual_hash = sha256_file(patch_path)
            if actual_hash != expected_hash:
                raise RuntimeError(
                    f"Patch integrity check failed for {patch_path}: "
                    f"expected {expected_hash}, found {actual_hash}"
                )

            relative = patch_path.relative_to(root)
            check = subprocess.run(
                git_args("apply", "--check", str(patch_path)),
                cwd=repo,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if check.returncode != 0:
                detail = (check.stdout or "").strip()
                if detail:
                    print(detail, file=sys.stderr, flush=True)
                raise RuntimeError(f"Patch no longer applies cleanly: {relative}")

            run_checked(
                git_args("apply", str(patch_path)),
                repo,
                f"Applying {relative}",
            )
            print(f"[OK] {dependency['name']}: {relative}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--repair", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    if lock.get("schemaVersion") != 1 or not isinstance(lock.get("dependencies"), list):
        raise RuntimeError("unsupported or malformed dependencies.lock.json")

    # Parent dependencies first. N64Recomp is managed by the runtime submodule
    # and is pinned explicitly immediately afterwards.
    for dep in lock["dependencies"]:
        if dep.get("managedBy"):
            continue
        checkout_dependency(root, dep, args.repair)

    managed = next(d for d in lock["dependencies"] if d["name"] == "n64recomp")
    n64recomp = root / managed["destination"]
    if not n64recomp.exists():
        raise RuntimeError("N64Recomp submodule was not materialised by N64ModernRuntime")

    run(git_args("config", "core.autocrlf", "false"), cwd=n64recomp)
    run(git_args("config", "core.eol", "lf"), cwd=n64recomp)
    run(git_args("reset", "--hard"), cwd=n64recomp)
    run(git_args("clean", "-ffd"), cwd=n64recomp)
    if has_commit(n64recomp, str(managed["commit"])):
        print(f"= using locally cached pinned commit for n64recomp: {managed['commit']}", flush=True)
    else:
        run_checked(
            git_args("fetch", "--no-tags", "--force", "origin", str(managed["commit"])),
            n64recomp,
            "Fetching pinned N64Recomp commit",
        )
    run(git_args("checkout", "--detach", str(managed["commit"])), cwd=n64recomp)
    run(git_args("reset", "--hard", str(managed["commit"])), cwd=n64recomp)
    run(git_args("clean", "-ffd"), cwd=n64recomp)
    run(git_args("submodule", "sync", "--recursive"), cwd=n64recomp)
    run_checked(
        git_args("submodule", "update", "--init", "--recursive", "--force"),
        n64recomp,
        "Populating N64Recomp submodules",
    )

    apply_patches(root)
    print("Dependencies prepared, pinned and patched successfully.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
