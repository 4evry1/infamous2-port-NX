"""Replay the shadow-caster fixture using the binary built by test_sparse_uploads.py."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    traces = []
    for enabled in ("0", "1"):
        trace = Path(tmp) / enabled
        env = dict(os.environ, DELTA_SHADOW_FIXTURE="1", PS3RECOMP_SHADOW_TIMING=enabled,
                   ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
        result = subprocess.run([binary, trace, "1"], env=env, capture_output=True, text=True, check=True)
        traces.append(trace.read_bytes())
        rows = [dict(re.findall(r"(\w+)=([0-9.-]+)", line)) for line in result.stderr.splitlines()
                if line.startswith("[shadow-time]")]
        if enabled == "0":
            assert not rows
        else:
            first = rows[0]
            assert first["frame"] == "1" and first["attempts"] == first["draws"] == "12"
            assert float(first["explicit_gpu_wait_ms"]) >= 1.19
            assert float(first["prep_cpu_ms"]) == -1  # recorder has no CPU residency clock
            for key in ("prep_nonwait_wall_ms", "draw_and_tail_nonwait_wall_ms"):
                assert 0 <= float(first[key]) < 10000
            assert len(rows) >= 2 and rows[1]["frame"] == "31" and rows[1]["attempts"] == "0"
    assert traces[0] == traces[1]
print("shadow selection, sampling, wait accounting and render parity passed")
