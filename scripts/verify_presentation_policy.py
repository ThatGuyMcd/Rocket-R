#!/usr/bin/env python3
"""Read-only checks for explicit interpolation callsites and generated hooks."""
import argparse
import json
from pathlib import Path
import re

from generate_recomp_config import policy_integer, validate_policy


def verify(root: Path, with_generated: bool) -> None:
    policy = json.loads((root / "runtime-recomp/rocket.us.recomp-policy.json").read_text())
    validate_policy(policy)
    hooks = policy["functionHooks"]
    callers = [h for h in hooks if "callsiteTarget" in h]
    entries = [h for h in hooks if "callsiteEntry" in h]
    targets = {0x8001ECEC, 0x8008B24C, 0x800476CC, 0x8004A4F0}
    if {policy_integer(h["callsiteEntry"]) for h in entries} != targets or len(entries) != 4:
        raise ValueError("expected exactly four callsite entry guards")
    if len(callers) != 39 or {policy_integer(h["callsiteTarget"]) for h in callers} != targets:
        raise ValueError("expected all 39 verified retail callsites")
    for hook in callers:
        address, target = hook["beforeVram"], hook["callsiteTarget"]
        if f"rocket_presentation_callsite(ctx, {target}U, {address}U);" not in hook["text"]:
            raise ValueError(f"missing explicit callsite bridge at {address}")
    for hook in entries:
        target = hook["callsiteEntry"]
        if policy_integer(hook["beforeVram"]) != policy_integer(target):
            raise ValueError(f"callsite guard is not at entry: {target}")
        if not hook["text"].startswith(f"/* ROCKET-R POLICY V44 ENTER {target} */"):
            raise ValueError(f"callsite guard must run before other entry hooks: {target}")
    source = (root / "src/presentation_identity.cpp").read_text()
    if "context->r31" in source:
        raise ValueError("interpolation still reads unreliable guest r31")
    if with_generated:
        files = sorted((root / "runtime-recomp/RecompiledFuncs").glob("*.c"))
        corpus = "\n".join(path.read_text() for path in files)
        for hook in callers + entries:
            if hook["text"] not in corpus:
                raise ValueError(f"generated hook missing/changed: {hook['function']}/{hook['beforeVram']}")
        # Discover direct calls independently from the recompiler's instruction
        # comments. This catches a missing policy callsite when the input changes.
        actual = set()
        for match in re.finditer(r"// (0x[0-9A-Fa-f]+): jal\s+(0x[0-9A-Fa-f]+)", corpus):
            address, target = (int(value, 16) for value in match.groups())
            if target in targets:
                actual.add((address, target))
        expected = {(policy_integer(h["beforeVram"]), policy_integer(h["callsiteTarget"])) for h in callers}
        if actual != expected:
            raise ValueError(f"generated callsite coverage differs: missing={actual - expected}, stale={expected - actual}")
    print("[OK] Explicit interpolation callsites: 39 JAL sites, 4 entry guards; no guest-r31 dependency.")
    if with_generated:
        print("[OK] Generated callsite hooks and complete direct-call coverage verified read-only.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--with-generated", action="store_true")
    args = parser.parse_args()
    verify(args.root.resolve(), args.with_generated)
