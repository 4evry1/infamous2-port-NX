"""Summarize a render-thread target capture with the exact archived ELF."""
import argparse
import bisect
import collections
import json
import re
import statistics
import subprocess
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    lines = args.capture.read_text().splitlines()
    # The trace format is independent of the build that recorded it.
    version = re.match(r"^# hz-0\.(\d{1,6}) ", lines[0]) if lines else None
    if not version or int(version[1]) < 107:
        parser.error("expected a supported render-thread target capture")
    meta = dict(field.split("=") for field in lines[0].split()[2:])
    begin, end = int(meta["begin"]), int(meta["end"])
    events, samples = [], []
    for line in lines[1:]:
        tag, *fields = line.split("\t")
        if tag == "E":
            ns, cpu, phase, frame, draws = fields
            events.append((int(ns), int(cpu), phase, int(frame), int(draws)))
        elif tag == "P":
            ns, resumed, pc, lr = fields
            samples.append((int(ns), int(resumed), int(pc,16), int(lr,16)))
        else:
            parser.error("unexpected row")
    if len(events) != int(meta["events"]) or len(samples) != int(meta["samples"]):
        parser.error("incomplete capture")
    symbols = []
    binary = subprocess.check_output([args.nm,"-n","--defined-only",str(args.elf)], text=True)
    for row in binary.splitlines():
        fields = row.split(None,2)
        if len(fields)==3 and fields[1] in ("T","t","W","w"):
            symbols.append((int(fields[0],16),fields[2]))
    addresses = [a for a,_ in symbols]
    base = int(meta["ref"],16) - next(a for a,n in symbols if n=="__libnx_exception_handler")
    def resolve(pc):
        address = pc-base
        i = bisect.bisect_right(addresses,address)-1
        return symbols[i][1] if 0<=i<len(symbols) and address<=addresses[-1] else "(outside ELF text)"
    times = [e[0] for e in events]
    assert times == sorted(times) and begin < end
    by_phase, drain_pc, drain_lr = collections.Counter(), collections.Counter(), collections.Counter()
    for ns, _, pc, lr in samples:
        i = bisect.bisect_right(times, ns)-1
        phase = events[i][2] if i>=0 else "before_first_event"
        if int(meta["dropped"]) and i == len(events)-1:
            phase = "after_trace_overflow"
        by_phase[phase] += 1
        if phase == "drain":
            drain_pc[resolve(pc)] += 1
            drain_lr[resolve(lr)] += 1
    # First drain after each wake. Discard partial windows at either end.
    wake = None
    gaps, cpus = [], []
    phase_wall, phase_cpu = collections.Counter(), collections.Counter()
    for left,right in zip(events,events[1:]):
        if left[2] == "wake":
            wake = left
            wall, cpu = collections.Counter(), collections.Counter()
        if wake is None:
            continue
        if left[2] in ("wait","drain"):
            wake = None
            continue
        segment = left[2] if left[2] in ("tick","present","pump","flip_check") else "other"
        wall[segment] += right[0]-left[0]
        cpu_valid = left[1] <= right[1] < 2**64-1
        if cpu_valid:
            cpu[segment] += right[1]-left[1]
        if right[2] == "drain":
            gaps.append((right[0]-wake[0])/1e6)
            phase_wall.update(wall)
            if wake[1] <= right[1] < 2**64-1:
                cpus.append((right[1]-wake[1])/1e6)
                phase_cpu.update(cpu)
            wake = None
    frames = [e[4] for e in events if e[2]=="frame"]
    def stats(v):
        if not v: return {"count":0}
        return {"count":len(v),"min":min(v),"median":statistics.median(v),"max":max(v)}
    print(json.dumps({"metadata":meta,"seconds":(end-begin)/1e9,"frame_draws":stats(frames),
        "sample_observation_us":stats([(r-n)/1e3 for n,r,_,_ in samples]),
        "pc_samples_by_phase":by_phase,"drain_pc_counts":drain_pc.most_common(20),
        "drain_lr_counts":drain_lr.most_common(20),"wake_to_first_drain_ms":stats(gaps),
        "wake_to_first_drain_cpu_ms":stats(cpus),
        "wake_phase_wall_ms_total":{k:v/1e6 for k,v in phase_wall.items()},
        "wake_phase_cpu_ms_total":{k:v/1e6 for k,v in phase_cpu.items()}},indent=2))

if __name__ == "__main__":
    main()
