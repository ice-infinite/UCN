#!/usr/bin/env python3
"""Exercise stack upper bounds through the real checker CLI."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).with_name("check_v6_call_stack.py")


class CallStackTests(unittest.TestCase):
    def run_gate(self, nodes, edges=(), limit=768, source="/src/core/a.c"):
        with tempfile.TemporaryDirectory(prefix="ucn-call-stack-") as directory:
            records = []
            for name, size, kind in nodes:
                suffix = "" if size is None else f"\\n{size} bytes ({kind})"
                records.append(
                    f'node: {{ title: "{name}" label: "{name}\\n{source}:1{suffix}" }}'
                )
            records.extend(
                f'edge: {{ sourcename: "{source}" targetname: "{target}" }}'
                for source, target in edges
            )
            Path(directory, "fixture.ci").write_text("\n".join(records), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(CHECKER), "--build-dir", directory,
                 "--limit-bytes", str(limit)],
                capture_output=True, text=True, check=False,
            )

    def test_static_and_proven_bounded_exact_limit(self):
        for qualifier in ("static", "dynamic,bounded"):
            with self.subTest(qualifier=qualifier):
                result = self.run_gate(
                    [("caller", 512, "static"), ("callee", 256, qualifier)],
                    [("caller", "callee")],
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("maximum=768 limit=768", result.stdout)

    def test_bound_is_counted_not_skipped(self):
        result = self.run_gate(
            [("caller", 513, "static"), ("callee", 256, "dynamic,bounded")],
            [("caller", "callee")],
        )
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("maximum=769 limit=768", result.stdout)

    def test_windows_source_separators(self):
        for source in (r"E:\project\src\core\a.c", r"E:\\project\\src\\core\\a.c"):
            with self.subTest(source=source):
                result = self.run_gate([("a", 768, "static")], source=source)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("functions=1 maximum=768 limit=768", result.stdout)

    def test_unknown_or_unbounded_rejected(self):
        for qualifier in ("dynamic", "bounded", "unknown", "static,bounded"):
            with self.subTest(qualifier=qualifier):
                result = self.run_gate([("a", 16, qualifier)])
                self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotEqual(self.run_gate([("a", None, "")]).returncode, 0)
        self.assertNotEqual(self.run_gate([]).returncode, 0)

    def test_recursion_and_conflicting_records_rejected(self):
        self.assertEqual(self.run_gate(
            [("a", 16, "static")], [("a", "a")]
        ).returncode, 1)
        self.assertEqual(self.run_gate(
            [("a", 16, "static"), ("a", 32, "static")]
        ).returncode, 1)


if __name__ == "__main__":
    unittest.main()
