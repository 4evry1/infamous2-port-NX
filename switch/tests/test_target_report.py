"""Check wake-phase accounting, runtime relocation and rejection of partial captures."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

report = Path(__file__).resolve().parents[1] / "tools/hz_target_report.py"
with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    (d/"symbols.c").write_text("void __libnx_exception_handler(void) {}\nvoid draw_fixture(void) {}\nint main(void){ return 0; }\n")
    subprocess.run(["cc","-O0",d/"symbols.c","-o",d/"symbols"],check=True)
    names = {a[2]:int(a[0],16) for line in subprocess.check_output(["nm","-n",d/"symbols"],text=True).splitlines()
             if len(a:=line.split())==3 and a[1] in ("T","t")}
    base = 0x80000000
    ref = base+names["__libnx_exception_handler"]
    pc = base+names["draw_fixture"]
    rows = [f"# hz-0.107 ref={ref:x} begin=100 end=10000000 target=6400 samples=1 events=6 dropped=0",
            "E\t1000\t100\twake\t7\t6400", "E\t2000\t200\tpresent\t7\t6400",
            "E\t8000\t300\tpresent_done\t7\t6400","E\t9000\t350\tframe\t8\t6402",
            "E\t10000\t500\tdrain\t8\t6402","E\t20000\t10400\tdrain_done\t8\t6402",
            f"P\t12000\t12200\t{pc:x}\t{ref:x}"]
    path=d/"capture.tsv"
    cmd=[sys.executable,report,path,d/"symbols"]
    path.write_text("\n".join(rows)+"\n")
    info=json.loads(subprocess.check_output(cmd,text=True))
    assert info["drain_pc_counts"]==[["draw_fixture",1]]
    assert info["wake_to_first_drain_ms"]["median"]==0.009
    assert info["wake_to_first_drain_cpu_ms"]["median"]==0.0004
    assert info["wake_phase_wall_ms_total"]["present"]==0.006
    assert info["wake_phase_cpu_ms_total"]["present"]==0.0001
    for version in ("hz-0.108", "hz-0.109", "hz-0.110", "hz-0.111", "hz-0.112", "hz-0.113"):
        path.write_text("\n".join(rows).replace("# hz-0.107 ", f"# {version} ")+"\n")
        assert json.loads(subprocess.check_output(cmd,text=True)) == info
    path.write_text("\n".join(rows[:-1])+"\n")
    assert subprocess.run(cmd,capture_output=True).returncode != 0
