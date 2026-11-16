"""Where every thread's time goes in a PS3_FTRACE timeline, and what the main thread waits on per frame.

    python3 switch/tools/ftrace_crit.py <ftrace.txt> [--top 12] [--thread NAME ...]

Input: the engine's PS3_FTRACE text (engine/include/ps3emu/ps3_ftrace.h), e.g. a run's
logs/ftrace.txt. Span kinds used: 1 syscall (who = PPU tid, a = number, b = guest lr), 2 merged usleeps
(a = calls, b = lr), 3 SPU channel wait (who = SPU, a = channel, b = LS pc), 4 SPU busy (a = image), 5/6 RSX
blocked on a semaphore acquire, 7 RSX FIFO drain, 9 flip requested by the guest, 10 host present,
12 CPU waiting for the GPU, 18 SPU job-module call (needs PS3_JOBSTAT=1; a = job overlay id), 19 SPU wake.

What it prints:
  * frames: flips requested and presented in the window, and the frame interval spread;
  * per PPU thread: the share of the window spent blocked in lv2 (syscalls and usleep polls), and the top
    blocking call sites by total time (syscall name @ guest lr). The rest of the window is "not blocked":
    running, or ready but waiting for a core (the trace cannot tell those apart; the sampling profile can);
  * per SPU thread: busy share by lifted image, channel-wait share, and the job modules by total time;
  * the RSX thread: FIFO drain time, semaphore waits, GPU waits;
  * the main thread frame by frame (flip to flip): mean blocked time per call site, so a frame's
    critical path reads as "main waited N ms in X on average".
Spans shorter than PS3_FTRACE_MIN_US (default 20 us) are not in the trace, so blocked times are a slight
undercount.
"""
import argparse
import bisect
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
try:
    import tracediff as _td
    SC_NAMES = _td.syscall_names()
except Exception:   # the names are a convenience only
    SC_NAMES = {}


def sc_name(n):
    return SC_NAMES.get(n, 'syscall_%d' % n)


def load(path):
    names, rows, head = {}, [], ''
    for ln in open(path, errors='replace'):
        p = ln.split()
        if not p:
            continue
        if p[0] == '#':
            head = ln.strip()
            continue
        if p[0] == 'T':
            names[int(p[1])] = ' '.join(p[2:])
            continue
        if len(p) < 6:
            continue
        rows.append((int(p[0]), int(p[1]), int(p[2], 16), int(p[3], 16), float(p[4]) / 1e3, float(p[5]) / 1e3))
    return head, names, rows


def pct(x, w):
    return 100.0 * x / w if w else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ftrace')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--thread', action='append', default=[], help='PPU thread names to list (default: all that block)')
    a = ap.parse_args()
    head, names, rows = load(a.ftrace)
    if not rows:
        print('no spans'); return
    # The recorded window is the header's; spans that began before it (a long wait already running) are cut
    # to it, or the window would stretch back to boot.
    import re as _re
    m = _re.search(r'start_us=([\d.]+) end_us=([\d.]+)', head)
    t_lo = float(m.group(1)) / 1e3 if m else min(r[4] for r in rows)
    t_hi = float(m.group(2)) / 1e3 if m else max(r[5] for r in rows)
    t_hi = min(t_hi, max(r[5] for r in rows))
    rows = [(k, w, x, b, max(t0, t_lo), min(t1, t_hi)) for k, w, x, b, t0, t1 in rows if t1 > t_lo and t0 < t_hi]
    W = t_hi - t_lo
    print(head)
    print('window %.0f ms (%.3f s .. %.3f s), %d spans' % (W, t_lo / 1e3, t_hi / 1e3, len(rows)))

    # ---- frames
    flips = sorted(r[4] for r in rows if r[0] == 9)
    pres = sorted(r[4] for r in rows if r[0] == 10)
    def spread(ts):
        d = sorted(b - a_ for a_, b in zip(ts, ts[1:]))
        if not d:
            return 'n/a'
        q = lambda f: d[min(len(d) - 1, int(f * len(d)))]
        return 'mean %.1f ms, p50 %.1f, p90 %.1f, max %.1f' % (sum(d) / len(d), q(0.5), q(0.9), d[-1])
    print('\n== frames')
    print('  flips requested %d (%.2f/s): %s' % (len(flips), len(flips) / (W / 1e3), spread(flips)))
    print('  presented       %d (%.2f/s): %s' % (len(pres), len(pres) / (W / 1e3), spread(pres)))

    # ---- PPU threads
    blocked = collections.defaultdict(float)
    sites = collections.defaultdict(lambda: collections.defaultdict(lambda: [0.0, 0]))
    for k, who, x, lr, t0, t1 in rows:
        if k == 1:
            key = '%s@%08X' % (sc_name(x), lr)
        elif k == 2:
            key = 'usleep-poll@%08X' % lr
        else:
            continue
        d = t1 - t0
        blocked[who] += d
        s = sites[who][key]; s[0] += d; s[1] += (x if k == 2 else 1)
    print('\n== PPU threads: share of the window blocked in lv2 (the rest is running or waiting for a core)')
    want = set(a.thread)
    for tid in sorted(blocked, key=lambda t: blocked[t]):
        nm = names.get(tid, 'tid%d' % tid)
        if want and nm not in want and str(tid) not in want:
            continue
        print('  %-30s blocked %5.1f%%  not blocked %5.1f%%' % ('%s (%d)' % (nm, tid), pct(blocked[tid], W), 100 - pct(blocked[tid], W)))
    for tid in sorted(blocked, key=lambda t: blocked[t]):
        nm = names.get(tid, 'tid%d' % tid)
        if want and nm not in want and str(tid) not in want:
            continue
        if not want and pct(blocked[tid], W) > 97:
            continue   # an idle helper: blocked nearly all the time, nothing to learn
        top = sorted(sites[tid].items(), key=lambda kv: -kv[1][0])[:a.top if want or nm == 'main' else 4]
        print('  -- %s (%d):' % (nm, tid))
        for key, (ms, n) in top:
            print('       %7.1f ms %5.1f%%  x%-6d %s' % (ms, pct(ms, W), n, key))

    # ---- SPU threads
    busy = collections.defaultdict(lambda: collections.defaultdict(float))
    chw = collections.defaultdict(lambda: collections.defaultdict(float))
    jobs = collections.defaultdict(lambda: [0.0, 0])
    for k, who, x, b, t0, t1 in rows:
        if k == 4:
            busy[who][x] += t1 - t0
        elif k == 3:
            chw[who][x] += t1 - t0
        elif k == 18:
            j = jobs[x]; j[0] += t1 - t0; j[1] += 1
    if busy or chw:
        print('\n== SPU threads (busy = running lifted code; wait = blocked on a channel)')
        for spu in sorted(set(busy) | set(chw)):
            tb = sum(busy[spu].values()); tw = sum(chw[spu].values())
            imgs = ', '.join('img %d %.0f%%' % (i, pct(v, W)) for i, v in sorted(busy[spu].items(), key=lambda kv: -kv[1])[:4])
            chs = ', '.join('ch%d %.0f%%' % (c & 0xFF, pct(v, W)) for c, v in sorted(chw[spu].items(), key=lambda kv: -kv[1])[:3])
            print('  spu %-5d busy %5.1f%% (%s)  wait %5.1f%% (%s)' % (spu, pct(tb, W), imgs, pct(tw, W), chs))
    if jobs:
        tot = sum(v[0] for v in jobs.values())
        print('  job modules (PS3_JOBSTAT), by total time across all SPUs (%.0f ms = %.2f SPU-seconds per second):' % (tot, tot / W))
        for ovl, (ms, n) in sorted(jobs.items(), key=lambda kv: -kv[1][0])[:a.top]:
            print('     ovl %-5d %8.1f ms  %5.1f%% of job time  x%-6d %.0f us each' % (ovl, ms, pct(ms, tot), n, 1e3 * ms / n))

    # ---- RSX thread
    rs = collections.defaultdict(lambda: [0.0, 0])
    for k, who, x, b, t0, t1 in rows:
        if k in (5, 6, 7, 12):
            r = rs[k]; r[0] += t1 - t0; r[1] += 1
    if rs:
        label = {5: 'semaphore acquire wait', 6: 'semaphore wait that gave up', 7: 'FIFO drain passes', 12: 'CPU waiting for the GPU'}
        print('\n== RSX / renderer')
        for k in (7, 5, 6, 12):
            if k in rs:
                print('  %-30s %8.1f ms %5.1f%%  x%d' % (label[k], rs[k][0], pct(rs[k][0], W), rs[k][1]))

    # ---- main thread per frame
    main_tid = next((t for t, n in names.items() if n == 'main'), None)
    if main_tid is not None and len(flips) > 2:
        per = collections.defaultdict(float)
        nfr = len(flips) - 1
        f0, f1 = flips[0], flips[-1]
        for k, who, x, lr, t0, t1 in rows:
            if who != main_tid or k not in (1, 2):
                continue
            lo, hi = max(t0, f0), min(t1, f1)
            if hi <= lo:
                continue
            key = ('%s@%08X' % (sc_name(x), lr)) if k == 1 else ('usleep-poll@%08X' % lr)
            per[key] += hi - lo
        span = (f1 - f0) / nfr
        tb = sum(per.values()) / nfr
        print('\n== main thread per frame (%d frames, %.1f ms each): blocked %.1f ms, not blocked %.1f ms' % (nfr, span, tb, span - tb))
        for key, ms in sorted(per.items(), key=lambda kv: -kv[1])[:a.top]:
            print('     %6.2f ms/frame  %s' % (ms / nfr, key))


if __name__ == '__main__':
    main()
