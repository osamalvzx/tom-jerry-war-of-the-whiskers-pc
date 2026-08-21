# Read a soak's logcat + thermal samples and answer ONE question: does the frame time hold,
# and when it does not, what was the device doing? Frame windows and clock samples are joined
# on WALL CLOCK, because the only honest way to blame a clock drop for a slow window is to
# show they happened at the same moment.
import re, sys, time, datetime, os
tag = sys.argv[1] if len(sys.argv) > 1 else "hot"
out = os.path.expandvars(r"%LOCALAPPDATA%\Temp\tj_soak")
log = os.path.join(out, "soak_%s.log" % tag)
th  = os.path.join(out, "therm_%s.log" % tag)

# --- clock/thermal samples: epoch -> (big max GHz, prime max GHz, zone C, batt C, cores) ---
samples = []
for ln in open(th, errors="replace"):
    m = re.match(r"t=(\d+)s (\d+) (.*)", ln.strip())
    if not m: continue
    ep = int(m.group(2)); rest = m.group(3)
    cur = {int(c): (int(a), int(b)) for c, a, b in
           re.findall(r"c(\d+)=(\d+)/(\d+)", rest)}
    zone = re.search(r"thr=(\d+)", rest); batt = re.search(r"batt=(\d+)", rest)
    cores = re.findall(r"([\w.\-]+):cpu(\d+):pri(-?\d+)", rest)
    samples.append(dict(t=int(m.group(1)), ep=ep, cur=cur,
                        zone=int(zone.group(1))/1000.0 if zone and zone.group(1) else None,
                        batt=int(batt.group(1))/10.0 if batt and batt.group(1) else None,
                        cores=cores))

def clock_at(ep):
    if not samples: return None
    return min(samples, key=lambda s: abs(s["ep"] - ep))

# --- frame windows ---
WIN = re.compile(r"^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d+).*frame (\d+): ([\d.]+)ms/f "
                 r"\[pb ([\d.\-]+) draw ([\d.\-]+) tex ([\d.\-]+) swap ([\d.\-]+) rest ([\d.\-]+)\].*"
                 r"3d=(\d+).*stut (\d+)/(\d+)/(\d+) max=([\d.]+)")
SPIKE = re.compile(r"frame (\d+): ([\d.]+)ms \[pb ([\d.\-]+) draw ([\d.\-]+) tex ([\d.\-]+) "
                   r"swap ([\d.\-]+) rest ([\d.\-]+)\] 3d=(\d+) ui=(\d+) texcre=(\d+) texupd=(\d+)")
yr = datetime.datetime.now().year
rows, spikes = [], []
for ln in open(log, errors="replace"):
    m = WIN.search(ln)
    if m:
        g = m.groups()
        dt = datetime.datetime(yr, int(g[0]), int(g[1]), int(g[2]), int(g[3]), int(g[4]))
        rows.append(dict(ep=int(time.mktime(dt.timetuple())), frame=int(g[6]), ms=float(g[7]),
                         pb=float(g[8]), draw=float(g[9]), tex=float(g[10]), swap=float(g[11]),
                         rest=float(g[12]), d3d=int(g[13]),
                         o17=int(g[14]), o20=int(g[15]), o33=int(g[16]), mx=float(g[17])))
        continue
    s = SPIKE.search(ln)
    if s:
        g = s.groups()
        spikes.append(dict(frame=int(g[0]), ms=float(g[1]), pb=float(g[2]), draw=float(g[3]),
                           tex=float(g[4]), swap=float(g[5]), rest=float(g[6]), d3d=int(g[7]),
                           cre=int(g[9]), upd=int(g[10])))

inm = [r for r in rows if r["d3d"] > 0]
print("windows: %d total, %d in-match (3d>0)" % (len(rows), len(inm)))
if inm:
    t0 = inm[0]["ep"]
    print("\n  min:s  frame    ms/f   rest  swap   over17 over20 over33   worst   big/prime max GHz  zone")
    for r in inm:
        c = clock_at(r["ep"]); el = r["ep"] - t0
        big = c["cur"].get(4, (0, 0))[1] / 1e6 if c else 0
        pri = c["cur"].get(7, (0, 0))[1] / 1e6 if c else 0
        print("  %3d:%02d %6d  %6.2f %6.2f %5.2f   %4d   %4d   %4d  %6.1f  %4d   %.2f/%.2f  %.2f/%.2f  %s"
              % (el // 60, el % 60, r["frame"], r["ms"], r["rest"], r["swap"],
                 r["o17"], r["o20"], r["o33"], r["mx"], r["d3d"],
                 (c["cur"].get(4,(0,0))[0]/1e6) if c else 0, big,
                 (c["cur"].get(7,(0,0))[0]/1e6) if c else 0, pri,
                 ("%.1fC" % c["zone"]) if c and c["zone"] else "?"))
    n = len(inm)
    print("\n  in-match summary over %d windows (%d frames):" % (n, n * 400))
    print("    ms/f  mean %.2f  min %.2f  max %.2f" %
          (sum(r["ms"] for r in inm) / n, min(r["ms"] for r in inm), max(r["ms"] for r in inm)))
    for k, lbl in (("o17", ">17.5ms"), ("o20", ">20ms"), ("o33", ">33ms")):
        tot = sum(r[k] for r in inm)
        print("    frames %-8s %6d of %6d  (%.2f%%)" % (lbl, tot, n * 400, 100.0 * tot / (n * 400)))
    print("    worst single frame: %.1f ms" % max(r["mx"] for r in inm))
if spikes:
    print("\n  %d spike lines; worst 12 by ms:" % len(spikes))
    for s in sorted(spikes, key=lambda x: -x["ms"])[:12]:
        dom = max((("rest", s["rest"]), ("swap", s["swap"]), ("draw", s["draw"]),
                   ("tex", s["tex"]), ("pb", s["pb"])), key=lambda kv: kv[1])
        print("    frame %-7d %6.1f ms  dominant %-4s %5.1f  (3d=%d texcre=%d)"
              % (s["frame"], s["ms"], dom[0], dom[1], s["d3d"], s["cre"]))
    inmS = [s for s in spikes if s["d3d"] > 0]
    if inmS:
        import collections
        c = collections.Counter(max((("rest", s["rest"]), ("swap", s["swap"]), ("draw", s["draw"]),
                                     ("tex", s["tex"]), ("pb", s["pb"])), key=lambda kv: kv[1])[0]
                                for s in inmS)
        print("    in-match spikes by dominant phase: %s" % dict(c))
if samples:
    print("\n  clock/thermal track:")
    for s in samples:
        cores = " ".join("%s@cpu%s" % (n, c) for n, c, p in s["cores"]
                         if n.strip("()") in ("tjgame", "BootThread", "eng", "Thread-2")) or ""
        print("    t=%4ds  c0 %.2f/%.2f  c4 %.2f/%.2f  c7 %.2f/%.2f  zone %s  batt %s  %s" % (
            s["t"], s["cur"].get(0,(0,0))[0]/1e6, s["cur"].get(0,(0,0))[1]/1e6,
            s["cur"].get(4,(0,0))[0]/1e6, s["cur"].get(4,(0,0))[1]/1e6,
            s["cur"].get(7,(0,0))[0]/1e6, s["cur"].get(7,(0,0))[1]/1e6,
            ("%.1fC" % s["zone"]) if s["zone"] else "?",
            ("%.1fC" % s["batt"]) if s["batt"] else "?", cores))
