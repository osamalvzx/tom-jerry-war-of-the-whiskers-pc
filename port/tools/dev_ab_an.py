# Compare the interleaved device legs written by dev_ab_session.sh.
# Reads `rest` (the sim's own time) and the stutter counters rather than ms/f: the pacer caps
# the frame at 16.67 ms, so a faster sim buys HEADROOM and fewer late frames, not a lower mean.
# insn/f is printed beside everything as the workload invariant -- if it differs between legs
# the scenes were not comparable and the comparison is void (device-perf-measurement-rules).
import re, os, sys, statistics
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
out = os.path.expandvars(r"%LOCALAPPDATA%\Temp\tj_soak")
PH   = re.compile(r"\[pb ([\d.\-]+) draw ([\d.\-]+) tex ([\d.\-]+) swap ([\d.\-]+) rest ([\d.\-]+)\]")
MS   = re.compile(r"frame \d+: ([\d.]+)ms/f")
D3D  = re.compile(r"3d=(\d+) ")
ACQ  = re.compile(r"acq=(\d+)/(\d+) ")
TAIL = re.compile(r"insn=(\d+)k/f gate=\d+/f texdec ([\d.]+)/texupl ([\d.]+) \((\d+)kpx/f\) "
                  r"regime\[recomp (\d+) conflict (\d+) flush (\d+) stale (\d+) decl (\d+) evict (\d+)\] "
                  r"stut (\d+)/(\d+)/(\d+) max=([\d.]+)")
def leg(tag):
    p = os.path.join(out, "soak_%s.log" % tag)
    rows = []
    if not os.path.exists(p): return rows
    for ln in open(p, errors="replace"):
        t=TAIL.search(ln); ph=PH.search(ln); d=D3D.search(ln); m=MS.search(ln); a=ACQ.search(ln)
        if not (t and ph and d and m): continue
        if int(d.group(1)) < 100: continue          # in-match only
        rows.append(dict(ms=float(m.group(1)), rest=float(ph.group(5)), draw=float(ph.group(2)),
                         tex=float(ph.group(3)), swap=float(ph.group(4)), insn=int(t.group(1)),
                         dec=float(t.group(2)), conflict=int(t.group(6)), recomp=int(t.group(5)),
                         decl=int(t.group(9)), evict=int(t.group(10)), upd=int(a.group(2)) if a else 0,
                         o17=int(t.group(11)), o20=int(t.group(12)), o33=int(t.group(13)),
                         mx=float(t.group(14))))
    return rows
med = lambda r,k: statistics.median(x[k] for x in r)
pool = {}
print("\n  leg          win   ms/f   sim    draw   swap   insn/f   late>17.5  >20    worst  conflict  texupd")
for tag in ["sess_new_1","sess_old_1","sess_new_2","sess_old_2"]:
    r = leg(tag)
    if not r: print("  %-12s (no in-match windows)" % tag); continue
    pool.setdefault(re.sub(r"_\d+$","",tag), []).extend(r)
    n=len(r)
    print("  %-12s %3d %6.2f %6.2f %6.2f %6.2f %7dk %8.1f%% %5.1f%% %6.1f %8d %7d"
          % (tag, n, med(r,"ms"), med(r,"rest"), med(r,"draw"), med(r,"swap"), med(r,"insn"),
             100.0*sum(x["o17"] for x in r)/(400.0*n), 100.0*sum(x["o20"] for x in r)/(400.0*n),
             max(x["mx"] for x in r), sum(x["conflict"] for x in r), sum(x["upd"] for x in r)))
if len(pool) == 2:
    a, b = pool["sess_new"], pool["sess_old"]
    ia, ib = med(a,"insn"), med(b,"insn")
    print("\n  pooled NEW vs OLD (same binary, interleaved):")
    print("    workload   insn/f %6dk vs %6dk   %s" % (ia, ib,
          "comparable" if abs(ia-ib) < 0.08*max(ia,ib) else "NOT COMPARABLE -- void"))
    print("    sim time   %6.2f vs %6.2f ms/f   %.2fx" % (med(a,"rest"), med(b,"rest"),
          med(b,"rest")/med(a,"rest") if med(a,"rest") else 0))
    print("    frame      %6.2f vs %6.2f ms/f" % (med(a,"ms"), med(b,"ms")))
    for k,l in (("o17",">17.5ms"),("o20",">20ms"),("o33",">33ms")):
        print("    frames %-8s %6.2f%% vs %6.2f%%" % (l,
              100.0*sum(x[k] for x in a)/(400.0*len(a)), 100.0*sum(x[k] for x in b)/(400.0*len(b))))
    print("    conflicts  %6d vs %6d   (block-cache collisions)" % (
          sum(x["conflict"] for x in a), sum(x["conflict"] for x in b)))
    print("    tex updates%6d vs %6d   (per-frame re-decodes)" % (
          sum(x["upd"] for x in a), sum(x["upd"] for x in b)))
