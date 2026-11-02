"""Exercise the real installer logging/trap block without installing anything.

Run with Python 3 and Bash (BASH_EXE may select Git Bash on Windows).
All paths and macOS commands are redirected into a temporary test directory.
"""
import os
import itertools
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / "package/install_opencore_userspace.sh").read_text(encoding="utf-8")
BASH = os.environ.get("BASH_EXE") or (
    r"C:\Program Files\Git\bin\bash.exe" if os.name == "nt" else shutil.which("bash")
)


class InstallerLoggingTest(unittest.TestCase):
    def run_installer(self, status=0, esp=True, rtf_failure=False):
        with tempfile.TemporaryDirectory(prefix="arcmtl-log-test-") as directory:
            root = Path(directory)
            tmp = root / "tmp"
            tmp.mkdir()
            (root / "ArcMTL").mkdir()
            if esp:
                (root / "EFI/OC").mkdir(parents=True)
            # Execute the shipped startup and cleanup code, stopping before
            # archive extraction / installation. Only external dependencies and
            # absolute host paths are substituted.
            prefix = SCRIPT.split('if [ -d "$SCRIPT_DIR/out" ]; then', 1)[0]
            prefix = prefix.replace("/var/tmp", tmp.as_posix())
            for name in ("OPENCORE", "EFI", "ESP"):
                prefix = prefix.replace("/Volumes/" + name, (root / "absent" / name).as_posix())
            prefix = prefix.replace("/usr/bin/log show", "arc_test_kernel_log")
            python = shlex.quote(Path(sys.executable).as_posix())
            prelude = (
                "uname() { echo Darwin; }\n"
                "id() { echo 0; }\n"
                "diskutil() { return 1; }\n"
                "sync() { :; }\n"
                "arc_test_kernel_log() { echo 'kernel: final ArcXe marker'; }\n"
                + ("python3() { return 99; }\n" if rtf_failure else
                   f'python3() {{ {python} "$@"; }}\n')
            )
            harness = root / "ArcMTL/install.sh"
            harness.write_text(
                prelude + prefix + '\necho "stdout marker"\n'
                + "printf '%s\\n' 'stderr {\\} Tiếng Việt 😀' >&2\n"
                + f"exit {status}\n", encoding="utf-8", newline="\n",
            )
            result = subprocess.run(
                [BASH, harness.as_posix()], capture_output=True,
                encoding="utf-8", errors="replace", timeout=20,
            )
            self.assertEqual(result.returncode, status, result.stdout + result.stderr)
            raw = (tmp / "arcmtl_install.log").read_text(encoding="utf-8")
            for marker in ("stdout marker", "stderr {\\} Tiếng Việt 😀",
                           "kernel: final ArcXe marker", f"Installer exit status: {status}"):
                self.assertIn(marker, raw)
            if esp:
                self.assertEqual((root / "arcmtl_debug.log").read_text(encoding="utf-8"), raw)
                if not rtf_failure:
                    rtf = (root / "log.rtf").read_text(encoding="ascii")
                    self.assertTrue(rtf.startswith(r"{\rtf1"))
                    self.assertIn(r"\{\\\}", rtf)
                    self.assertIn(r"\u-10179?\u-8704?", rtf)
                    self.assertIn(f"Installer exit status: {status}", rtf)
                    self.assertTrue(rtf.endswith("}"))
                else:
                    self.assertIn("RTF export failed", result.stdout)
            else:
                self.assertIn("No writable OpenCore ESP", result.stdout)

    def test_success_drains_log_and_exports_unicode(self):
        self.run_installer()

    def test_failure_preserves_status_and_final_diagnostics(self):
        self.run_installer(status=17)

    def test_missing_esp_keeps_local_log(self):
        self.run_installer(status=19, esp=False)

    def test_rtf_failure_keeps_plaintext_and_original_status(self):
        self.run_installer(status=23, rtf_failure=True)

    def test_acceleration_requires_all_three_tests_even_with_force(self):
        start = SCRIPT.index('if [ "$MODE" = "--disable-arc-accel" ] || [ "$MODE" = "--safe-stage1" ]; then')
        gate = SCRIPT[start:]
        for mode in ("--enable-dual-metal", "--enable-arc-desktop"):
            for results in itertools.product((0, 1), repeat=3):
                with self.subTest(mode=mode, results=results):
                    prelude = (
                        'MODE="$1"\n'
                        'SCRIPT_DIR=/test\n'
                        f"PROBE_OK={results[0]} VK_OK={results[1]} BUNDLE_OK={results[2]}\n"
                        'install_auxkc_arcfb_arcaccel() { :; }\n'
                        'update_oc_config() { echo "CONFIG=$1"; }\n'
                    )
                    result = subprocess.run(
                        [BASH, "-s", "--", mode, "--force"], input=prelude + gate,
                        capture_output=True, text=True, timeout=10,
                    )
                    expected = mode.removeprefix("--enable-") if all(results) else "safe-stage1"
                    self.assertEqual(result.returncode, 0 if all(results) else 1, result.stderr)
                    self.assertIn(f"CONFIG={expected}", result.stdout)

    def test_boundary_diagnostics_never_promote_baseline_result(self):
        start = SCRIPT.index('# Run after ALL baseline GPU tests')
        block = SCRIPT[start:SCRIPT.index('stamp_host_deps() {', start)]
        for baseline, diag_status, available in itertools.product((0, 1), repeat=3):
            with self.subTest(baseline=baseline, diag_status=diag_status, available=available):
                runnable = '/bin/bash' if available else '/no-such-arc-test'
                code = block.replace('-x /usr/local/bin/vk_offscreen_test', f'-x {runnable}')
                code = code.replace('/usr/local/bin/vk_offscreen_test', 'arc_test_vk')
                prelude = (
                    f'VK_OK={baseline}\n'
                    'arc_test_vk() { echo "CALLED probes=$ARC_VK_BOUNDARY_PROBES"; '
                    f'return {diag_status}; }}\n'
                )
                result = subprocess.run(
                    [BASH, '-s'], input=prelude + code + '\necho "FINAL_VK_OK=$VK_OK"\n',
                    capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f'FINAL_VK_OK={baseline}', result.stdout)
                self.assertEqual('CALLED probes=1' in result.stdout, baseline == 0 and bool(available))


if __name__ == "__main__":
    unittest.main()
