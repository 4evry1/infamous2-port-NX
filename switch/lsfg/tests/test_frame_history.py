"""Builds and runs the host harness for rsx::lsfg::FrameHistory.

The LSFG adapter asks this policy, for every presentation, whether the frame is passed straight
through, only captured as warmup history, or generated. tests/frame_history.cpp drives the shipped
project/src/frame_history.h with explicit serials and monotonic millisecond stamps, so no clock and
no GPU is involved, and asserts the whole plan (decision and source-image index) per frame.

Run: python3 -I -B -m unittest discover -s tests -p test_lsfg_history.py -v
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE_DIR = ROOT
TEST_SOURCE = os.path.join(ROOT, "tests", "frame_history.cpp")
BOUNDED_SECONDS = 300
COMPILE_FLAGS = ["-std=c++23", "-O1", "-Wall", "-Wextra", "-Werror"]
SANITIZER_FLAGS = ["-std=c++23", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


def run(command, what, environment=None):
    """Runs a command, raising with its output when it fails."""
    result = subprocess.run(command, capture_output=True, text=True, timeout=BOUNDED_SECONDS,
                            env=environment)
    if result.returncode != 0:
        raise AssertionError(
            f"{what} failed (exit {result.returncode})\n"
            f"command: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def prepare_mutated_source(directory, name, replacements):
    """Copies the header next to a mutated version of it, returning its include directory."""
    include_dir = os.path.join(directory, name)
    os.makedirs(include_dir, exist_ok=True)
    with open(os.path.join(INCLUDE_DIR, "frame_history.h"), encoding="utf-8") as header:
        source = header.read()
    for original, mutated in replacements:
        if original not in source:
            raise AssertionError(f"mutation {name}: header no longer contains {original!r}")
        source = source.replace(original, mutated, 1)
    with open(os.path.join(include_dir, "frame_history.h"), "w", encoding="utf-8") as header:
        header.write(source)
    return include_dir


MUTATIONS = {
    # Warmup removed: the first accepted frame would generate immediately.
    "no_warmup": [("inline constexpr uint32_t kAcceptedFramesPerWarmup = 2;",
                   "inline constexpr uint32_t kAcceptedFramesPerWarmup = 0;")],
    # Duplicates accepted as new frames: a repeated serial would generate and reset nothing.
    "duplicates_accepted": [(
        "guest_serial != 0 && (!has_accepted_serial_ || guest_serial > last_accepted_serial_);",
        "guest_serial != 0 && (!has_accepted_serial_ || guest_serial >= last_accepted_serial_);")],
    # The warmup is restarted by a bypass, which would drop a duplicate into the warmup again.
    "duplicate_resets_warmup": [(
        "const bool is_repeated_serial =",
        "const bool is_repeated_serial = false &&")],
}


class LsfgHistoryTest(unittest.TestCase):
    """Compiles the harness once per build variant, then runs each."""

    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("g++")
        if not cls.compiler:
            raise AssertionError("missing host tool for the LSFG history test: g++")
        cls._temporary = tempfile.TemporaryDirectory(prefix="lsfg_history_")
        cls.addClassCleanup(cls._temporary.cleanup)
        cls.optimized_harness = cls.build_harness("optimized", INCLUDE_DIR, COMPILE_FLAGS)
        cls.sanitized_harness = cls.build_harness("sanitized", INCLUDE_DIR, SANITIZER_FLAGS)

    @classmethod
    def build_harness(cls, name, include_dir, flags):
        harness = os.path.join(cls._temporary.name, "lsfg_history_" + name)
        run([cls.compiler, *flags, "-I", include_dir, "-o", harness, TEST_SOURCE],
            f"compiling tests/frame_history.cpp ({name})")
        return harness

    def run_harness(self, harness):
        return run([harness], f"running tests/frame_history.cpp ({os.path.basename(harness)})")

    def assert_all_checks_passed(self, output):
        lines = output.splitlines()
        failures = [line for line in lines if line.startswith("FAIL ")]
        self.assertEqual(failures, [], output)
        self.assertIn("PASS ", output)
        self.assertTrue(lines and lines[-1].startswith("FAILURES "), output)
        self.assertEqual(lines[-1], "FAILURES 0", output)

    def test_optimized_build_passes_every_check(self):
        self.assert_all_checks_passed(self.run_harness(self.optimized_harness).stdout)

    def test_sanitized_build_passes_every_check(self):
        environment = dict(os.environ)
        environment.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
        result = run([self.sanitized_harness],
                     "running tests/frame_history.cpp (sanitized)", environment)
        self.assert_all_checks_passed(result.stdout)

    def test_ordinary_sequence_warms_then_generates(self):
        output = self.run_harness(self.optimized_harness).stdout
        self.assertIn("PASS ordinary: two warmup frames then generated", output)
        self.assertIn("PASS ordinary: the very first accepted frame never generates", output)

    def test_duplicates_bypass_without_touching_warmup(self):
        output = self.run_harness(self.optimized_harness).stdout
        self.assertIn("PASS duplicates: repeated serial bypasses", output)
        self.assertIn("PASS duplicates: a duplicate does not restart warmup", output)
        self.assertIn("PASS duplicates: a duplicate inside the warmup does not restart it", output)

    def test_disallowed_and_zero_frames_restart_history(self):
        output = self.run_harness(self.optimized_harness).stdout
        self.assertIn("PASS ui: a disallowed frame of an accepted serial passes through", output)
        self.assertIn("PASS ui: a disallowed frame restarts the warmup", output)
        self.assertIn("PASS zero: an inactive frame restarts the warmup", output)
        self.assertIn("PASS stale: conservatively restarts the warmup", output)

    def test_gap_and_clock_boundaries(self):
        output = self.run_harness(self.optimized_harness).stdout
        self.assertIn("PASS gap: exactly the maximum interval stays continuous", output)
        self.assertIn("PASS gap: one millisecond past the maximum restarts the warmup", output)
        self.assertIn("PASS clock: a regressing timestamp restarts the warmup without a huge gap "
                      "interpretation", output)

    def test_reset_and_large_serials(self):
        output = self.run_harness(self.optimized_harness).stdout
        self.assertIn("PASS reset: clears the accepted serial and the warmup", output)
        self.assertIn("PASS reset: repeated resets leave a fresh history", output)
        self.assertIn("PASS large: the maximum serial is a valid accepted serial", output)

    def test_mutations_are_caught(self):
        for name, replacements in MUTATIONS.items():
            with self.subTest(mutation=name):
                include_dir = prepare_mutated_source(self._temporary.name, name, replacements)
                harness = self.build_harness(name, include_dir, COMPILE_FLAGS)
                # A mutation must make the harness fail its checks: exit non-zero, never clean.
                result = subprocess.run([harness], capture_output=True, text=True,
                                        timeout=BOUNDED_SECONDS)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("FAIL ", result.stdout)
                self.assertNotEqual(result.stdout.splitlines()[-1], "FAILURES 0", result.stdout)


if __name__ == "__main__":
    unittest.main()
