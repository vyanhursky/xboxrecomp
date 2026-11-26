"""Ensure CTest cannot silently accept validation errors from either stream."""
from pathlib import Path
import subprocess
import sys
import unittest


class ValidationRunnerTest(unittest.TestCase):
    def run_child(self, code):
        return subprocess.run([sys.executable, str(Path(__file__).with_name('validation_run.py')),
                               sys.executable, '-c', code], capture_output=True, text=True)

    def test_clean_child(self):
        result = self.run_child("print('native pixel comparisons passed')")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('native pixel comparisons passed', result.stdout)

    def test_vuid_with_success_exit(self):
        result = self.run_child("print('VUID-vkCmdClearColorImage-image-00002: invalid usage')")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('invalid usage', result.stdout)

    def test_validation_error_on_stderr(self):
        result = self.run_child("import sys; print('Validation Error: bad resource', file=sys.stderr)")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('bad resource', result.stdout)

    def test_child_failure_is_preserved(self):
        self.assertEqual(self.run_child('raise SystemExit(7)').returncode, 7)


if __name__ == '__main__':
    unittest.main()
