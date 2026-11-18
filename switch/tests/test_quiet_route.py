"""Exercise the launcher's selected mode with the profiling flags used on the SD."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "horizon/src/hz_entry.c").read_text()
number = source.split("static unsigned long long env_number(", 1)[1].split("\n}", 1)[0]
mode = source.split("    from_file = s_env_from_file;\n", 1)[1].split("    /* The console keeps", 1)[0]
host = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long env_number(""" + number + "\n}\n" + """
int main(int argc, char **argv) {
    int s_quiet = 0;
""" + mode + """
    const char *names[] = {"PS3RECOMP_ROUTE_TIMING", "PS3RECOMP_SHADOW_TIMING"};
    printf("%d", s_quiet);
    for (int i = 0; i < 2; ++i) {
        const char *v = getenv(names[i]);
        printf(" %s", v ? v : "unset");
    }
}
"""
with tempfile.TemporaryDirectory() as temp:
    binary = Path(temp) / "launch"
    subprocess.run(["cc", "-std=gnu17", "-Wall", "-Werror", "-x", "c", "-", "-o", binary],
                   input=host, text=True, check=True)
    for filename, override, quiet in [("Infamous2Recomp.nro", None, False),
                                      ("Infamous2Recomp-quiet.nro", None, True),
                                      ("Infamous2Recomp-quiet.nro", "0", False),
                                      ("Infamous2Recomp.nro", "1", True)]:
        for flags in [("1", "1"), ("0", "1"), ("1", "0"), (None, None)]:
            env = dict(os.environ)
            for key, value in zip(("HZ_QUIET", "PS3RECOMP_ROUTE_TIMING", "PS3RECOMP_SHADOW_TIMING"),
                                  (override, *flags)):
                env.pop(key, None)
                if value is not None:
                    env[key] = value
            result = subprocess.check_output(["sdmc:/switch/infamous2-port/" + filename],
                                             executable=binary, env=env, text=True).split()
            assert result == [str(int(quiet)), *("unset" if quiet or v is None else v for v in flags)], result
print("Passed 16 logged/quiet filename, override and shared-settings cases")
