"""Turn a console sampling profile (logs/profile-N.bin, written by the Horizon layer when MINUS+PLUS
is pressed) into "which functions took the time", using the ELF of the same build.

Usage (needs devkitA64's nm; run inside the Switch dev container, or pass --nm):
    hz_profile.py <profile.bin> <build.elf> [--top 40] [--nm /path/to/aarch64-none-elf-nm]

Per thread: how many samples, and the share that was not sitting in a wait (a sample whose pc is
in a kernel-call stub or sleep is "waiting"). Then the busiest functions over all threads, by pc,
and the same by caller (lr) for samples that landed in small helpers."""
import bisect
import collections
import struct
import subprocess
import sys

args = sys.argv[1:]
top = int(args[args.index("--top") + 1]) if "--top" in args else 40
nm = args[args.index("--nm") + 1] if "--nm" in args else "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
prof, elf = args[0], args[1]

data = open(prof, "rb").read()
magic, code_ref, rate, threads = struct.unpack_from("<8sQQQ", data, 0)
assert magic.startswith(b"HZPROF1"), "not a profile file"
samples = [struct.unpack_from("<IIQQ", data, 32 + i * 24) for i in range((len(data) - 32) // 24)]

symbols = []
for line in subprocess.run([nm, "-n", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        symbols.append((int(p[0], 16), p[2]))
symbols.sort()
addresses = [a for a, _ in symbols]
handler = next(a for a, n in symbols if n == "__libnx_exception_handler")
base = code_ref - handler


def name_of(address):
    offset = address - base
    i = bisect.bisect_right(addresses, offset) - 1
    if i < 0 or offset - addresses[i] > 0x200000:
        return "?%x" % offset
    return symbols[i][1]


WAIT = ("svc", "mutexLock", "condvar", "__nx_", "threadWait", "nanosleep", "usleep", "sleep", "waitSingle",
        "eventWait", "semaphore", "pthread_cond", "_wait", "armGetSystemTick")
by_thread = collections.defaultdict(lambda: [0, 0])
by_pc = collections.Counter()
by_lr = collections.Counter()
busy_total = 0
for thread, _, pc, lr in samples:
    n = name_of(pc)
    waiting = any(w in n for w in WAIT)
    by_thread[thread][0] += 1
    if not waiting:
        by_thread[thread][1] += 1
        by_pc[n] += 1
        by_lr[name_of(lr)] += 1
        busy_total += 1

seconds = max(by_thread[t][0] for t in by_thread) / rate if by_thread else 0
print(f"{len(samples)} samples, {threads} threads known, about {seconds:.0f} s at {rate} Hz; "
      f"{busy_total} samples were not waiting = {busy_total / rate / max(seconds, 1e-9):.2f} cores busy on average")
print("threads by busy share (thread: busy% of its samples):")
for t, (n, busy) in sorted(by_thread.items(), key=lambda kv: -kv[1][1])[:16]:
    if busy:
        print(f"  thread {t:3d}: {100.0 * busy / n:5.1f}%")
print(f"busiest functions (share of all busy samples), top {top}:")
for n, c in by_pc.most_common(top):
    print(f"  {100.0 * c / max(busy_total, 1):5.1f}%  {n[:110]}")
print("by caller (lr), top 20:")
for n, c in by_lr.most_common(20):
    print(f"  {100.0 * c / max(busy_total, 1):5.1f}%  {n[:110]}")
