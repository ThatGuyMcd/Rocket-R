import copy
import importlib.util
import json
from pathlib import Path
import unittest
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("config", ROOT / "scripts/generate_recomp_config.py")
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class RecompPolicyTests(unittest.TestCase):
    def setUp(self):
        self.policy = json.loads((ROOT / "runtime-recomp/rocket.us.recomp-policy.json").read_text())

    def test_checked_policy(self):
        config.validate_policy(self.policy)

    def test_equivalent_address_duplicates(self):
        for field, key in (("functionHooks", "beforeVram"), ("instructionPatches", "vram")):
            with self.subTest(field=field):
                policy = copy.deepcopy(self.policy)
                duplicate = copy.deepcopy(policy[field][0])
                duplicate[key] = str(int(duplicate[key], 16))
                policy[field].append(duplicate)
                with self.assertRaisesRegex(ValueError, "duplicate"):
                    config.validate_policy(policy)

    def test_wrong_jal_target(self):
        hook = next(h for h in self.policy["functionHooks"] if "callsiteTarget" in h)
        hook["callsiteTarget"] = "0x80000400"
        with self.assertRaisesRegex(ValueError, "not a JAL"):
            config.validate_policy(self.policy)

    def test_instruction_bytes_checked(self):
        hook = copy.deepcopy(next(h for h in self.policy["functionHooks"] if "callsiteTarget" in h))
        hook["romOffset"] = 0
        policy = {"schemaVersion": 1, "functionHooks": [hook]}
        config.validate_policy(policy, int(hook["expectedInstruction"], 16).to_bytes(4, "big"))
        with self.assertRaisesRegex(ValueError, "does not match"):
            config.validate_policy(policy, b"\x00" * 4)

    def test_hook_text_round_trip(self):
        try:
            import tomllib
        except ModuleNotFoundError:
            import tomli as tomllib
        value = 'line 1\nline 2\t"quoted" \\ path'
        self.assertEqual(tomllib.loads("text = " + config.toml_string(value))["text"], value)

    def test_declaration_hook_after_branch_label_is_valid_c17(self):
        compiler = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable; Android build also exercises this boundary")
        hook = config.hook_statement("extern void track(void); track();")
        source = "void test(void) { goto target; target: " + hook + " }\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hook.c"
            path.write_text(source)
            result = subprocess.run([compiler, "-std=c17", "-pedantic-errors", "-fsyntax-only", str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
