#!/usr/bin/env python3
"""Where does the wall-clock time of the EOS M ML autofocus go?  (field-log time budget)

    python3 contrib/af_test/time_budget.py --logs ~/af_logs/af-*            # all sessions
    python3 contrib/af_test/time_budget.py --logs DIR [DIR ...] [--since-header 't=22339']
    python3 contrib/af_test/time_budget.py --logs DIR --section 1,5         # only some sections

Reads AFDRIVE.LOG and AFRTEST.LOG of every DIR (a folder with ML/LOGS, LOGS or the .LOG files).
Card logs are append-only, so later session copies contain the earlier runs: runs are deduplicated
by their `# AFDRIVE t=..` header line (+ the `# RESULT` line, a truncated copy loses against a
complete one).  Dirs are processed in name order, so give oldest to newest.

Time model (verified on the logs): the `<ms>` of a move line is the run-clock time at the END of that
command (move + wait + measure); a tracked chunk `# sweep|rc ... t_send= t_done=` starts at `t_send`
on the same clock.  So every ms of a run is attributed exactly once by walking a cursor:
  move line   : ms - cursor            -> motion (done_ms) / wait / samp / dispatch overhead
  chunk       : (t_send - cursor) gap  -> chunk gap ; t_done -> chunk motion
  first line  : session open + LV recover (minus its own measure)
  RESULT      : epilogue_ms is added after the last line.
`total_ms` of `# RESULT` is NOT used for totals (it excludes the session open); it is shown for reference.

Sections: 1 phase breakdown, 2 move-cost model, 3 frame interval, 4 measurement cost, 5 time sinks.
Stdlib only.
"""
import argparse
import collections
import glob
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

try:
    from field_session import find_logs_dir, read_lines, parse_kv
except Exception:  # pragma: no cover - keep the tool usable stand-alone
    def parse_kv(line):
        return {k: v for k, v in re.findall(r"(\w+)=([^\s]+)", line)}

    def read_lines(path):
        try:
            with open(path, errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def find_logs_dir(base):
        for cand in (os.path.join(base, "ML", "LOGS"), os.path.join(base, "LOGS"), base):
            if os.path.isdir(cand) and glob.glob(os.path.join(cand, "*.LOG")):
                return cand
        return os.path.join(base, "ML", "LOGS")

MOVE_RE = re.compile(
    r"^(\d+) ([A-Za-z]) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) ([0-9a-fA-F]+) (\S+) (-?\d+) ret(-?\d+)"
    r"(?: dist=(\d+))? t=(\d+) wait=(\d+) samp=(\d+) q=(-?\d+)")
CHUNK_RE = re.compile(r"^# (sweep|rc) dir=-?\d+ len=\d+")
S_RE = re.compile(r"^S (\d+) (-?\d+) (-?\d+) (-?\d+)")
TIMER_BIT, REFUSED_BIT = 0x0400, 0x1000
TIMER_MS = 480          # a done_ms this long is the ~510 ms FOCUS_DONE timer even if bit 0x0400 is clear
LAT_MS = 120            # sw_lat_ms (also parsed from the algo header when present)
BUCKETS = ["session", "local", "b_probe", "coarse1f", "sweep", "landing", "endstop", "other", "epilogue"]
BUCKET_NAMES = {
    "session": "session open + LV recover", "local": "Local stage (w/W/f + i)", "b_probe": "B probe (p + i)",
    "coarse1f": "coarse 1F scan", "sweep": "sweep (07, incl. o setup)",
    "landing": "landing (o/r/t/l, rc chunks)", "endstop": "end-stop probes (x)", "other": "other/unclassified",
    "epilogue": "epilogue"}


# ---------------------------------------------------------------- statistics helpers
def pct(vals, p):
    vals = sorted(vals)
    if not vals:
        return None
    k = (len(vals) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(vals) - 1)
    return vals[lo] + (vals[hi] - vals[lo]) * (k - lo)


def med(vals):
    return st.median(vals) if vals else None


def fmt(v, w=6, nd=0):
    if v is None:
        return "-".rjust(w)
    return ("%*.*f" % (w, nd, v))


def linfit(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my - b * mx, b


# ---------------------------------------------------------------- parsing
class Run:
    def __init__(self, src, header, pre):
        self.src = src                  # 'AFDRIVE' | 'AFRTEST'
        self.header = header
        self.hdr = parse_kv(header)
        self.pre = pre                  # lines between the previous run and this header
        self.lines = []
        self.events = []                # ('mv', d) | ('ck', d) | ('note', text)
        self.result = None
        self.result_line = None
        self.issues = []
        self.opts = {}                  # merged kv of "# focal_mm=", "# algo=" header lines
        self.epi_fast = False
        self.lv_recover_ms = None
        self.cls = None


def parse_runs(lines, src):
    runs, cur, pre = [], None, []
    for ln in lines:
        if ln.startswith("# AFDRIVE "):
            cur = Run(src, ln, pre)
            pre = []
            runs.append(cur)
            continue
        if cur is None:
            continue
        if cur.result is not None:       # after RESULT: belongs to the gap between runs
            pre.append(ln)
            continue
        cur.lines.append(ln)
        if ln.startswith("# RESULT"):
            cur.result_line = ln
            cur.result = parse_kv(ln)
            cur.result["_rtest_prep"] = "rtest_prep" in ln
            continue
        m = MOVE_RE.match(ln)
        if m:
            g = m.groups()
            d = dict(ms=int(g[0]), ph=g[1], cmd=int(g[2]), pos=int(g[3]), s=int(g[4]), g=int(g[5]),
                     luma=int(g[6]), val=int(g[7], 16), flags=g[8], done=int(g[9]), ret=int(g[10]),
                     tries=int(g[12]), wait=int(g[13]), samp=int(g[14]), q=int(g[15]))
            cur.events.append(("mv", d))
            continue
        if CHUNK_RE.match(ln):
            kv = parse_kv(ln)
            d = dict(kind="rc" if ln.startswith("# rc") else "sweep", spd=kv.get("spd", "7"),
                     len=int(kv.get("len", 0)), t_send=int(kv.get("t_send", 0)),
                     t_done=int(kv.get("t_done", 0)), val=int(kv.get("val", "0"), 16),
                     mspp_x10=int(kv.get("mspp_x10", 29)), moved=int(kv.get("moved", 0)),
                     n=int(kv.get("n", 0)), S=[], dir=int(kv.get("dir", 0)),
                     coarse=False)
            cur.events.append(("ck", d))
            continue
        m = S_RE.match(ln)
        if m:
            for typ, d in reversed(cur.events):
                if typ == "ck":
                    d["S"].append(tuple(int(x) for x in m.groups()))
                    break
            continue
        if ln.startswith("# "):
            cur.events.append(("note", ln))
            if ln.startswith("# focal_mm=") or ln.startswith("# algo="):
                cur.opts.update(parse_kv(ln))
            elif ln.startswith("# epilogue"):
                cur.epi_fast = ln.startswith("# epilogue fast")
            elif ln.startswith("# lv_recover"):
                cur.lv_recover_ms = int(parse_kv(ln).get("ms", -1))
    return runs


def dedup(runs):
    """runs: list in oldest-to-newest order.  Returns (kept, n_dups, n_conflicts)."""
    groups = collections.OrderedDict()
    for r in runs:
        groups.setdefault((r.src, r.header), []).append(r)
    kept, dups, conflicts = [], 0, 0
    for _, grp in groups.items():
        complete = {}
        for r in grp:
            if r.result_line is not None:
                complete.setdefault(r.result_line, r)
        if complete:
            kept.extend(complete.values())
            dups += len(grp) - len(complete)
            conflicts += len(complete) - 1
        else:
            kept.append(max(grp, key=lambda r: len(r.lines)))
            dups += len(grp) - 1
    return kept, dups, conflicts


def load(dirs, since_header=None):
    allruns, rt_runs, rt_begins = [], [], []
    for base in dirs:
        ld = find_logs_dir(os.path.expanduser(base))
        for src in ("AFDRIVE", "AFRTEST"):
            lines = read_lines(os.path.join(ld, src + ".LOG"))
            if not lines:
                continue
            allruns.extend(parse_runs(lines, src))
            if src == "AFRTEST":
                for ln in lines:
                    if ln.startswith("# RT_RUN "):
                        rt_runs.append(ln)
                    elif ln.startswith("# RTEST_BEGIN"):
                        rt_begins.append(ln)
    if since_header:
        out, on = [], {}
        for r in allruns:
            if since_header in r.header:
                on[r.src] = True
            if on.get(r.src):
                out.append(r)
        allruns = out
    kept, dups, conflicts = dedup(allruns)
    rt_runs = list(collections.OrderedDict.fromkeys(rt_runs))   # same RT_RUN line repeated in later copies
    return kept, dict(dups=dups, conflicts=conflicts, rt_runs=rt_runs, n_rt_begin=len(set(rt_begins)))


# ---------------------------------------------------------------- classification
def classify(runs, rt_runs):
    """run.cls = dict(type, algo, landing).  type: rtest | rtest_prep | auto | auto_slow | manual."""
    rt_keys = collections.Counter()
    for ln in rt_runs:
        kv = parse_kv(ln)
        rt_keys[(kv.get("land_s"), kv.get("moves"))] += 1
    matched = 0
    for r in runs:
        res = r.result or {}
        prep = bool(res.get("_rtest_prep"))
        landing = "recatch" if r.opts.get("landing") == "recatch" else "classic"
        algo = r.hdr.get("algo", "?")
        trig = any(l.startswith("# aref trig") or l.startswith("# hunt attempt") for l in r.pre)
        if r.src == "AFRTEST" or prep:
            typ = "rtest_prep"
        else:
            key = (res.get("land_s"), res.get("moves"))
            if rt_keys.get(key, 0) > 0:
                rt_keys[key] -= 1
                typ = "rtest"
                matched += 1
            elif r.epi_fast:
                typ = "auto"
            elif trig:
                typ = "auto_slow"
            else:
                typ = "manual"
        r.cls = dict(type=typ, algo=algo, landing=landing, trig=trig)
    return matched


# ---------------------------------------------------------------- time attribution
def move_speed(ph, r):
    if ph == "o":
        return r.opts.get("unm_speed", "1f")
    return "7"


def model_ms(pulses, spd, mspp_x10=None, lat=LAT_MS):
    if mspp_x10 is None:
        mspp_x10 = 10 if spd.lower() == "1f" else 29
    return lat + abs(pulses) * mspp_x10 / 10.0


# firmware "# phase name=.. t=.." markers (patch 12 / S5) -> bucket; used when present, else the inference below
PHASE_BUCKET = {"session": "session", "probe": "b_probe", "coarse": "coarse1f", "sweep": "sweep", "land": "landing",
                "top": "landing", "local": "local", "epilogue": "epilogue"}


def attribute(r):
    """Fills r.cells (list of dict bucket/comp/ms/...) and r.mv (move records), r.ck (chunk records)."""
    r.cells, r.mv, r.ck, r.wall, r.notes = [], [], [], None, collections.Counter()
    res = r.result or {}
    lat = int(r.opts.get("sw_lat_ms", LAT_MS))
    cursor = 0
    state = "init"
    first = True
    coarse_pending = False
    prev = None                         # previous event type
    prev_mv_end = None
    local_fallback = False
    cur_phase = None                    # bucket named by the last "# phase" marker (None = no marker yet)
    ev = [e for e in r.events]
    for idx, (typ, d) in enumerate(ev):
        if typ == "note":
            t = d
            if t.startswith("# phase "):
                cur_phase = PHASE_BUCKET.get(parse_kv(t).get("name"), cur_phase)
            if t.startswith("# coarse1f: probe flat") or t.startswith("# coarse1f:") and "scan at" in t:
                coarse_pending = True
            if t.startswith("# local") and "-> B" in t:
                state = "b"
                local_fallback = True
            continue
        if typ == "mv":
            ph = d["ph"]
            wall = max(0, d["ms"] - cursor)
            if d["ms"] < cursor:
                r.issues.append("clock goes backwards at ms=%d" % d["ms"])
            nxt = next((e for e in ev[idx + 1:] if e[0] != "note"), (None, None))
            if first:
                bucket = "session"
                first = False
                cells = [("session", wall - d["samp"] - d["wait"]), ("b_probe", d["samp"] + d["wait"])] \
                    if False else [("session", wall)]
                for b, ms in cells:
                    r.cells.append(dict(bucket=b, comp="session", ms=ms))
                cursor = d["ms"]
                prev, prev_mv_end = "mv", d["ms"]
                state = "init"
                rec = dict(d, wall=wall, bucket="session", spd=move_speed(ph, r), first=True, after_ck=False)
                r.mv.append(rec)
                continue
            after_ck = prev == "ck"
            if ph in ("w", "W", "f"):
                bucket = "local"
                state = "local" if state in ("init", "local") else state
            elif ph == "p":
                bucket = "b_probe"
                state = "b"
            elif ph in ("r", "t", "l"):
                bucket = "landing"
                state = "landing"
            elif ph == "x":
                bucket = "endstop"
            elif ph == "i":
                if after_ck or state == "landing":
                    bucket = "landing"
                elif state == "local":
                    bucket = "local"
                else:
                    bucket = "b_probe"
            elif ph == "o":
                if nxt[0] == "ck":
                    bucket = "coarse1f" if (coarse_pending or nxt[1]["spd"].lower() == "1f") else "sweep"
                else:
                    bucket = "landing"
            else:
                bucket = "other"
                r.notes["unknown phase " + ph] += 1
            if cur_phase in BUCKETS and cur_phase not in ("session", "epilogue") and ph != "x":
                bucket = cur_phase
            speed = move_speed(ph, r)
            motion = d["done"] if d["done"] > 0 else 0
            wait, samp = d["wait"], d["samp"]
            over = wall - motion - wait - samp
            if over < -2 and motion:
                # done_ms can overlap the wait; keep the sum exact by clamping at 0 and shrinking motion
                motion = max(0, wall - wait - samp)
                over = wall - motion - wait - samp
            chunk_max = 300 if speed.lower() == "1f" else 100
            nchunks = max(1, -(-abs(d["cmd"]) // chunk_max))
            if nchunks > 1:
                # done_ms is only the LAST chunk of a chunked move: the earlier chunks land in "overhead"
                extra_model = (nchunks - 1) * model_ms(chunk_max, speed, None, lat)
                ocomp = "multichunk"
            else:
                extra_model = 0
                ocomp = "post_ck_overhead" if after_ck else "overhead"
            for comp, ms in (("motion", motion), ("wait", wait), ("samp", samp), (ocomp, over)):
                r.cells.append(dict(bucket=bucket, comp=comp, ms=ms, phase=ph, cmd=abs(d["cmd"]), spd=speed,
                                    extra_model=extra_model))
            rec = dict(d, wall=wall, bucket=bucket, spd=speed, first=False, after_ck=after_ck,
                       over=over, prev_mv_end=prev_mv_end, nchunks=nchunks)
            if after_ck:
                rec["wall_isolated"] = None
            r.mv.append(rec)
            cursor = d["ms"]
            prev, prev_mv_end = "mv", d["ms"]
            continue
        # chunk
        c = d
        spd = c["spd"].lower()
        if spd == "1f" or coarse_pending and c["kind"] == "sweep" and spd == "1f":
            bucket = "coarse1f"
        elif c["kind"] == "rc":
            bucket = "landing"
        else:
            bucket = "sweep"
        if cur_phase in BUCKETS and cur_phase not in ("session", "epilogue"):
            bucket = cur_phase
        if c["kind"] == "sweep" and spd != "1f":
            coarse_pending = False
        gap = c["t_send"] - cursor
        if gap < 0:
            r.issues.append("chunk t_send %d before cursor %d" % (c["t_send"], cursor))
            gap = 0
        done = max(0, c["t_done"])
        r.cells.append(dict(bucket=bucket, comp="ck_gap", ms=gap))
        r.cells.append(dict(bucket=bucket, comp="ck_motion", ms=done, cmd=c["len"], spd=spd,
                            model=model_ms(c["len"], spd, c["mspp_x10"], lat)))
        c["bucket"], c["gap"] = bucket, gap
        c["lat"] = lat
        r.ck.append(c)
        cursor = max(cursor, c["t_send"] + done)
        prev = "ck"
    epi = int(res.get("epilogue_ms", 0)) if res and res.get("epilogue_ms", "").lstrip("-").isdigit() else 0
    if res.get("_rtest_prep"):
        epi = 0
    r.cells.append(dict(bucket="epilogue", comp="epilogue", ms=epi))
    r.epilogue_ms = epi
    r.run_clock_ms = cursor
    r.wall = cursor + epi
    r.local_fallback = local_fallback
    r.bsum = collections.Counter()
    for c in r.cells:
        r.bsum[c["bucket"]] += c["ms"]
    return r


# ---------------------------------------------------------------- report sections
def group_table(title, runs):
    """Phase breakdown table for a set of runs."""
    n = len(runs)
    out = ["", "%s   (n=%d)" % (title, n)]
    if not n:
        return out
    tot_wall = sum(r.wall for r in runs)
    out.append("  %-34s %7s %7s %6s %7s %5s" % ("phase", "median", "p90", "share", "sum_s", "n>0"))
    for b in BUCKETS:
        vals = [r.bsum.get(b, 0) for r in runs]
        s = sum(vals)
        out.append("  %-34s %s %s %5.1f%% %s %5d" % (BUCKET_NAMES[b], fmt(med(vals), 7), fmt(pct(vals, 90), 7),
                                                    100.0 * s / tot_wall if tot_wall else 0, fmt(s / 1000.0, 7, 1),
                                                    sum(1 for v in vals if v > 0)))
    walls = [r.wall for r in runs]
    out.append("  %-34s %s %s %5.1f%% %s" % ("TOTAL wall (run clock+epilogue)", fmt(med(walls), 7), fmt(pct(walls, 90), 7),
                                             100.0, fmt(tot_wall / 1000.0, 7, 1)))
    tms = [int(r.result["total_ms"]) for r in runs if r.result and r.result.get("total_ms", "").isdigit()]
    if tms:
        out.append("  (RESULT total_ms median %d, excludes session open)" % med(tms))
    return out


def section1(runs):
    out = ["=" * 100, "1. PER-RUN PHASE BREAKDOWN (ms; median/p90 over the runs of the group, zeros included)", "=" * 100]
    typ_names = collections.OrderedDict([
        ("manual", "manual AF drive"), ("rtest", "Refocus-test trial runs"),
        ("auto", "auto-refocus/hunt, epilogue fast"), ("auto_slow", "auto-refocus triggered, slow (old) epilogue"),
        ("rtest_prep", "Refocus-test prep (algo=A, defocus reposition; no epilogue logged)")])
    out.append("run counts (after dedup): " + ", ".join("%s=%d" % (typ_names[t], sum(1 for r in runs if r.cls["type"] == t))
                                                       for t in typ_names))
    for t, name in typ_names.items():
        sub = [r for r in runs if r.cls["type"] == t]
        out += group_table("[type] " + name, sub)
    out.append("")
    out.append("--- by algo (all AF runs, rtest_prep excluded) ---")
    real = [r for r in runs if r.cls["type"] != "rtest_prep"]
    for a in ("L", "B"):
        out += group_table("[algo] " + a, [r for r in real if r.cls["algo"] == a])
    out.append("")
    out.append("--- by landing (all AF runs) ---")
    for ld in ("classic", "recatch"):
        out += group_table("[landing] " + ld, [r for r in real if r.cls["landing"] == ld])
    out.append("")
    out.append("--- type x algo (n, median wall ms) ---")
    cross = collections.defaultdict(list)
    for r in real:
        cross[(r.cls["type"], r.cls["algo"], r.cls["landing"])].append(r.wall)
    for k in sorted(cross):
        out.append("  %-10s algo=%s landing=%-7s n=%3d  median wall %6.0f  p90 %6.0f" %
                   (k[0], k[1], k[2], len(cross[k]), med(cross[k]), pct(cross[k], 90)))
    out.append("")
    out.append("--- median ms per phase for every type/algo/landing group (n>=1) ---")
    out.append("  %-10s %-2s %-7s %3s | %s | %6s" % ("type", "al", "landing", "n", " ".join("%7s" % b[:7] for b in BUCKETS), "wall"))
    grp = collections.defaultdict(list)
    for r in real:
        grp[(r.cls["type"], r.cls["algo"], r.cls["landing"])].append(r)
    for k in sorted(grp):
        g = grp[k]
        out.append("  %-10s %-2s %-7s %3d | %s | %6.0f" % (k[0], k[1], k[2], len(g),
                   " ".join("%7.0f" % med([r.bsum.get(b, 0) for r in g]) for b in BUCKETS), med([r.wall for r in g])))
    # Local stage fallback
    out.append("")
    out.append("--- Local stage: how often it ends in the B fallback, and what it cost ---")
    loc = [r for r in real if r.cls["algo"] == "L" and any(m["ph"] in ("w", "W", "f") for m in r.mv)]
    fb = [r for r in loc if r.local_fallback]
    ok = [r for r in loc if not r.local_fallback]
    out.append("  algo=L runs that attempted Local (>=1 w/W/f move): %d; ended in '-> B' fallback: %d (%.0f%%)" %
               (len(loc), len(fb), 100.0 * len(fb) / len(loc) if loc else 0))
    for name, grp in (("fallback to B", fb), ("Local succeeded", ok)):
        if not grp:
            continue
        lv = [r.bsum.get("local", 0) for r in grp]
        sh = [100.0 * r.bsum.get("local", 0) / r.wall for r in grp if r.wall]
        wl = [r.wall for r in grp]
        nm = [sum(1 for m in r.mv if m["bucket"] == "local") for r in grp]
        out.append("  %-16s n=%3d  Local time median %5.0f p90 %5.0f sum %6.1f s | share of run wall median %4.1f%% | "
                   "Local moves median %.0f | run wall median %.0f" %
                   (name, len(grp), med(lv), pct(lv, 90), sum(lv) / 1000.0, med(sh), med(nm), med(wl)))
    for t in ("manual", "rtest", "auto", "auto_slow"):
        g = [r for r in loc if r.cls["type"] == t]
        if g:
            f = [r for r in g if r.local_fallback]
            fl = [r.bsum.get("local", 0) for r in f]
            out.append("    %-9s Local runs %3d, fallback %3d (%3.0f%%), fallback Local cost median %s p90 %s ms" %
                       (t, len(g), len(f), 100.0 * len(f) / len(g), fmt(med(fl), 0), fmt(pct(fl, 90), 0)))
    reasons = collections.Counter()
    for r in fb:
        for typ, d in r.events:
            if typ == "note" and d.startswith("# local") and "-> B" in d:
                reasons[re.sub(r"[\d.]+", "N", d.split("# local:", 1)[-1].strip()) if "# local:" in d else "(local line)"] += 1
                break
    for k, v in reasons.most_common():
        out.append("      fallback reason: %-60s %d" % (k[:60], v))
    return out


def mark_reversals(runs):
    """m['rev'] = this move line reverses the direction of the previous (non-measure) move line."""
    for r in runs:
        last = 0
        for m in r.mv:
            if m["ph"] == "i" or m.get("first") or m["cmd"] == 0:
                m["rev"] = False
                continue
            s = 1 if m["cmd"] > 0 else -1
            m["rev"] = bool(last) and s != last
            last = s


def tmr_like(m):
    return bool(m["val"] & TIMER_BIT) or m["done"] >= TIMER_MS


def section2(runs):
    mark_reversals(runs)
    out = ["", "=" * 100, "2. PER-MOVE COST MODEL", "=" * 100]
    allmv = [m for r in runs for m in r.mv if m["ph"] != "i" and not m.get("first")]
    mv = [m for m in allmv if m["nchunks"] == 1]
    out.append("move lines (excluding measure-only i): %d, of which single-chunk (|p|<=100 at 07, <=300 at 1F): %d; "
               "tracked chunks: %d" % (len(allmv), len(mv), sum(len(r.ck) for r in runs)))
    out.append("Longer moves are split into chunks and the line's done_ms is only the LAST chunk, so they are excluded from the "
               "per-pulse tables.")
    out.append("done_ms is from the move line; wall = ms - previous line's ms (only when the previous event is a move line, "
               "so tracked chunks do not pollute it); ovh = wall - done - wait - samp.")
    out.append("'timer-like' = bit 0x0400 set OR done_ms >= %d (the ~510 ms FOCUS_DONE timer; the bit is not always set when "
               "the timer fires, e.g. r -28 -> done 501, val 4e0000). ref = bit 0x1000 (refused/end stop)." % TIMER_MS)
    out.append("")
    out.append("--- by phase (single-chunk move lines) ---")
    out.append("  %-3s %-4s %5s %6s %7s %7s %7s %7s %7s %6s %6s" % ("ph", "spd", "n", "|p|med", "done_md", "done_p90", "wall_md",
                                                                    "wall90", "ovh_md", "tmr%", "ref%"))
    byph = collections.defaultdict(list)
    for m in mv:
        byph[(m["ph"], m["spd"])].append(m)
    for k in sorted(byph):
        g = byph[k]
        d = [m["done"] for m in g if m["done"] >= 0]
        w = [m["wall"] for m in g if not m["after_ck"]]
        ov = [m["over"] for m in g if "over" in m and not m["after_ck"]]
        tm = sum(1 for m in g if tmr_like(m))
        rf = sum(1 for m in g if m["val"] & REFUSED_BIT)
        out.append("  %-3s %-4s %5d %6s %s %s %s %s %s %5.0f%% %5.0f%%" % (
            k[0], k[1], len(g), fmt(med([abs(m["cmd"]) for m in g]), 6), fmt(med(d), 7), fmt(pct(d, 90), 7), fmt(med(w), 7),
            fmt(pct(w, 90), 7), fmt(med(ov), 7), 100.0 * tm / len(g), 100.0 * rf / len(g)))
    multi = [m for m in allmv if m["nchunks"] > 1]
    if multi:
        out.append("  multi-chunk move lines: %d, median |p| %s, median wall %s ms (not in the tables below)" %
                   (len(multi), fmt(med([abs(m["cmd"]) for m in multi]), 0), fmt(med([m["wall"] for m in multi]), 0)))
    thr_summary = []
    for spd, label in (("7", "speed 07 (refused moves excluded)"),
                       ("1f", "speed 1F (the unmeasured 'o' moves; assumes o = unm_speed)")):
        out.append("")
        out.append("--- done_ms vs |pulses|, %s ---" % label)
        out.append("  %6s %5s %7s %7s %7s %6s | %-13s | %-13s | %7s %7s" %
                   ("|p|", "n", "done_md", "done_p10", "done_p90", "tmr%", "same-dir n/tmr", "reversal n/tmr", "wall_md", "ovh_md"))
        byp = collections.defaultdict(list)
        for m in mv:
            if m["spd"] == spd and not (m["val"] & REFUSED_BIT) and m["done"] >= 0:
                byp[abs(m["cmd"])].append(m)
        xs, ys = [], []
        rows = []
        for p in sorted(byp):
            g = byp[p]
            if len(g) < 3:
                continue
            d = [m["done"] for m in g]
            same = [m for m in g if not m["rev"]]
            rev = [m for m in g if m["rev"]]

            def tpc(x):
                return ("%3d/%3.0f%%" % (len(x), 100.0 * sum(1 for m in x if tmr_like(m)) / len(x))) if x else "   -"
            tmr = 100.0 * sum(1 for m in g if tmr_like(m)) / len(g)
            w = [m["wall"] for m in g if not m["after_ck"]]
            ov = [m["over"] for m in g if "over" in m and not m["after_ck"]]
            out.append("  %6d %5d %s %s %s %5.0f%% | %-13s | %-13s | %s %s" % (
                p, len(g), fmt(med(d), 7), fmt(pct(d, 10), 7), fmt(pct(d, 90), 7), tmr, tpc(same), tpc(rev),
                fmt(med(w), 7), fmt(med(ov), 7)))
            rows.append((p, tmr, same, rev))
        for m in mv:
            if m["spd"] == spd and not tmr_like(m) and not (m["val"] & REFUSED_BIT) and m["done"] >= 0:
                xs.append(abs(m["cmd"]))
                ys.append(m["done"])
        # pooled bins
        bins = [(0, 10), (11, 20), (21, 30), (31, 45), (46, 70), (71, 100)] if spd == "7" else \
               [(0, 60), (61, 100), (101, 140), (141, 200), (201, 300)]
        out.append("  pooled bins (|p|: n, timer-like % all / same-dir / reversal, done_ms median of the timer-like and of the "
                   "completion-driven moves):")
        first_bin = None
        for lo, hi in bins:
            g = [m for p, _, s_, r_ in rows for m in (s_ + r_) if lo <= p <= hi]
            if not g:
                continue
            tm = [m for m in g if tmr_like(m)]
            nt = [m for m in g if not tmr_like(m)]
            sm = [m for m in g if not m["rev"]]
            rv = [m for m in g if m["rev"]]
            f_all = 100.0 * len(tm) / len(g)
            f_s = 100.0 * sum(1 for m in sm if tmr_like(m)) / len(sm) if sm else None
            f_r = 100.0 * sum(1 for m in rv if tmr_like(m)) / len(rv) if rv else None
            out.append("    %3d-%-3d n=%4d  timer-like %3.0f%% / %s / %s   done_md timer %s, completion %s" % (
                lo, hi, len(g), f_all, ("%3.0f%%" % f_s) if f_s is not None else "  - ", ("%3.0f%%" % f_r) if f_r is not None else "  - ",
                fmt(med([m["done"] for m in tm]), 5), fmt(med([m["done"] for m in nt]), 5)))
            if first_bin is None and f_all >= 50:
                first_bin = (lo, hi)
        fit = linfit(xs, ys)
        if fit:
            out.append("  completion-driven moves (not timer-like): done_ms ~= %.0f + %.2f * |pulses|  (n=%d)" % (fit[0], fit[1], len(xs)))
        thr_summary.append("%s: first pooled bin with >=50%% timer-like = %s" % (
            "07" if spd == "7" else "1F", ("%d-%d pulses" % first_bin) if first_bin else "none"))
    out.append("")
    out.append("THRESHOLD SUMMARY: " + "; ".join(thr_summary) + ".  Timer-like completion is a mix, not a sharp cut "
               "(see the same-dir / reversal split above).")
    out.append("")
    out.append("--- direction reversal effect on done_ms (spd 07, single-chunk, |p|<=28) ---")
    rev = {True: [], False: []}
    for m in mv:
        if m["spd"] == "7" and 0 < abs(m["cmd"]) <= 28 and not (m["val"] & REFUSED_BIT) and m["done"] >= 0:
            rev[m["rev"]].append(m["done"])
    for k in (False, True):
        out.append("  %-14s n=%4d  done_ms median %s p90 %s" % ("reversal" if k else "same direction", len(rev[k]),
                                                              fmt(med(rev[k]), 0), fmt(pct(rev[k], 90), 0)))
    # chunks
    out.append("")
    out.append("--- tracked chunks: done (t_done), completion bit, idle ---")
    out.append("idle = (next t_send - this t_send) - (lat %d + len*mspp); next chunk must follow with no move line in between" % LAT_MS)
    out.append("gap  = next t_send - (t_send + t_done): dead time between the end of one chunk and the start of the next")
    out.append("  %-6s %-4s %5s %5s %7s %7s %7s %7s | %5s %7s %7s %7s %9s" %
               ("kind", "spd", "len", "n", "done_md", "t_exp", "tmr%", "ref%", "n_id", "idle_md", "idle_p90", "gap_md", "sum_idle_s"))
    groups = collections.defaultdict(list)
    for r in runs:
        for i, c in enumerate(r.ck):
            groups[(c["kind"], c["spd"].lower(), c["len"])].append((r, i, c))
    tot_idle = 0
    for k in sorted(groups):
        g = groups[k]
        idles, gaps = [], []
        for r, i, c in g:
            if i + 1 < len(r.ck):
                nxt = r.ck[i + 1]
                # was a move line between them?  yes iff some mv event timestamp lies between t_send's
                between = any(c["t_send"] < m["ms"] <= nxt["t_send"] for m in r.mv)
                if k[0] == "sweep" and between:
                    continue
                if k[0] == "rc" and nxt["kind"] != "rc":
                    continue
                gapv = nxt["t_send"] - c["t_send"]
                idles.append(gapv - model_ms(c["len"], k[1], c["mspp_x10"], c["lat"]))
                gaps.append(nxt["t_send"] - (c["t_send"] + max(0, c["t_done"])))
        d = [c["t_done"] for _, _, c in g]
        tm = 100.0 * sum(1 for _, _, c in g if c["val"] & TIMER_BIT) / len(g)
        rf = 100.0 * sum(1 for _, _, c in g if c["val"] & REFUSED_BIT) / len(g)
        tot_idle += sum(x for x in idles if x > 0)
        out.append("  %-6s %-4s %5d %5d %s %7d %6.0f%% %6.0f%% | %5d %s %s %s %s" % (
            k[0], k[1], k[2], len(g), fmt(med(d), 7), model_ms(k[2], k[1], g[0][2]["mspp_x10"], g[0][2]["lat"]),
            tm, rf, len(idles), fmt(med(idles), 7), fmt(pct(idles, 90), 7), fmt(med(gaps), 7), fmt(sum(x for x in idles if x > 0) / 1000.0, 9, 1)))
    out.append("  (t_exp column = modelled lat+len*mspp; rc chunks have a rest-measure i between them, so rc idle includes that i)")
    # done_ms vs t_done of chunks 100 pulses at 07: threshold scan
    out.append("")
    out.append("--- chunk completion by length at 07 (tracked chunks) ---")
    byl = collections.defaultdict(list)
    for r in runs:
        for c in r.ck:
            if c["spd"].lower() == "7":
                byl[c["len"]].append(c)
    for l in sorted(byl):
        g = byl[l]
        d = [c["t_done"] for c in g]
        out.append("  len %4d n=%4d t_done median %s p10 %s p90 %s, 0x0400 set %.0f%%" % (
            l, len(g), fmt(med(d), 0), fmt(pct(d, 10), 0), fmt(pct(d, 90), 0),
            100.0 * sum(1 for c in g if c["val"] & TIMER_BIT) / len(g)))
    return out


def frame_intervals(runs):
    res = collections.defaultdict(lambda: ([], []))   # spd -> (raw diffs, distinct-frame diffs)
    for r in runs:
        for c in r.ck:
            S = c["S"]
            lastt, lastv, lastchange = None, None, None
            for t, s, g, lu in S:
                if lastt is not None and t > lastt:
                    res[c["spd"].lower()][0].append(t - lastt)
                v = (s, g)
                if lastv is not None and v != lastv:
                    if lastchange is not None and t > lastchange:
                        res[c["spd"].lower()][1].append(t - lastchange)
                    lastchange = t
                elif lastv is None:
                    lastchange = t
                lastt, lastv = t, v
    return res


def section3(runs):
    out = ["", "=" * 100, "3. FRAME INTERVAL DURING TRACKED SWEEPS", "=" * 100]
    fi = frame_intervals(runs)
    out.append("S sample lines carry the time (ms from chunk start).  raw = between successive S lines; frame = between "
               "successive samples whose (s,g) CHANGED (a new LV frame).")
    out.append("  %-4s %6s %8s %8s %8s %8s %8s %12s" % ("spd", "mspp", "n_raw", "raw_med", "n_frm", "frm_med", "frm_p90", "pulses/frame"))
    for spd in ("7", "14", "1f"):
        raw, frm = fi.get(spd, ([], []))
        mspp = 2.9 if spd == "7" else (1.0 if spd == "1f" else None)
        if not raw and not frm:
            out.append("  %-4s %6s %8s  no chunks in the logs (only 07 and 1F are tracked)" % (spd, "-", "-"))
            continue
        m = med(frm)
        out.append("  %-4s %6.1f %8d %s %8d %s %s %12s" % (spd, mspp or 0, len(raw), fmt(med(raw), 8, 1), len(frm), fmt(m, 8, 1),
                                                          fmt(pct(frm, 90), 8, 1), ("%.1f" % (m / mspp)) if m and mspp else "-"))
    # per-chunk observed: moved pulses / duration
    out.append("  (pulses/frame = frame interval / ms-per-pulse from the chunk header mspp_x10: 2.9 at 07, 1.0 at 1F)")
    # distribution of the number of frames per chunk
    out.append("  frames per 100-pulse 07 chunk (distinct (s,g) samples): median %s" % fmt(med([
        len({(s, g) for t, s, g, lu in c["S"]}) for r in runs for c in r.ck if c["spd"].lower() == "7" and c["len"] == 100]), 0))
    return out


def section4(runs):
    out = ["", "=" * 100, "4. MEASUREMENT COST (wait= and samp= per phase; tries>1)", "=" * 100]
    mv = [m for r in runs for m in r.mv if not m.get("first")]
    out.append("  %-3s %6s | %6s %6s %6s | %6s %6s %6s | %8s %8s" %
               ("ph", "n", "wait50", "wait90", "waitmx", "samp50", "samp90", "sampmx", "tries>1", "wall_md"))
    byph = collections.defaultdict(list)
    for m in mv:
        byph[m["ph"]].append(m)
    for ph in sorted(byph):
        g = byph[ph]
        w = [m["wait"] for m in g]
        s = [m["samp"] for m in g]
        meas = [m for m in g if m["tries"] >= 1]
        gt = sum(1 for m in meas if m["tries"] > 1)
        out.append("  %-3s %6d | %s %s %s | %s %s %s | %4d/%-4d %s" % (
            ph, len(g), fmt(med(w), 6), fmt(pct(w, 90), 6), fmt(max(w), 6), fmt(med(s), 6), fmt(pct(s, 90), 6), fmt(max(s), 6),
            gt, len(meas), fmt(med([m["wall"] for m in g if not m["after_ck"]]), 8)))
    meas = [m for m in mv if m["tries"] >= 1]
    gt = [m for m in meas if m["tries"] > 1]
    out.append("")
    out.append("  measured moves (tries>=1): %d; tries>1 (lens unsettled -> re-measure): %d (%.1f%%); tries distribution: %s" %
               (len(meas), len(gt), 100.0 * len(gt) / len(meas) if meas else 0,
                dict(sorted(collections.Counter(m["tries"] for m in meas).items()))))
    if gt:
        out.append("  samp median: tries==1 %s ms, tries>1 %s ms (p90 %s); extra wall attributable to retries (samp of tries>1 minus "
                   "single-try median) = %.1f s total" %
                   (fmt(med([m["samp"] for m in meas if m["tries"] == 1]), 0), fmt(med([m["samp"] for m in gt]), 0),
                    fmt(pct([m["samp"] for m in gt], 90), 0),
                    sum(max(0, m["samp"] - med([x["samp"] for x in meas if x["tries"] == 1])) for m in gt) / 1000.0))
    out.append("  tries>1 by phase: " + ", ".join(
        "%s %d/%d" % (ph, sum(1 for m in byph[ph] if m["tries"] > 1), sum(1 for m in byph[ph] if m["tries"] >= 1))
        for ph in sorted(byph) if any(m["tries"] >= 1 for m in byph[ph])))
    # patch 11 rest-measure
    out.append("")
    out.append("--- patch 11 (landing=recatch): rest-measure 'i' frames after each rc chunk ---")
    rest, rc_end_wall = [], []
    for r in runs:
        if r.cls["landing"] != "recatch":
            continue
        for m in r.mv:
            if m["ph"] == "i" and m["after_ck"]:
                rest.append(m)
        # previous chunk: cost of the i = ms - (t_send+t_done) of last rc chunk before it
        for m in r.mv:
            if m["ph"] == "i" and m["after_ck"]:
                prev_c = [c for c in r.ck if c["t_send"] + max(0, c["t_done"]) <= m["ms"]]
                if prev_c:
                    c = prev_c[-1]
                    rc_end_wall.append(m["ms"] - (c["t_send"] + max(0, c["t_done"])))
    if rest:
        out.append("  n=%d  samp median %s p90 %s max %s | wait median %s | tries distribution %s | time from chunk end to line "
                   "median %s p90 %s | total %.1f s" % (
                       len(rest), fmt(med([m["samp"] for m in rest]), 0), fmt(pct([m["samp"] for m in rest], 90), 0),
                       fmt(max(m["samp"] for m in rest), 0), fmt(med([m["wait"] for m in rest]), 0),
                       dict(sorted(collections.Counter(m["tries"] for m in rest).items())),
                       fmt(med(rc_end_wall), 0), fmt(pct(rc_end_wall, 90), 0), sum(rc_end_wall) / 1000.0))
        rcn = [c for r in runs if r.cls["landing"] == "recatch" for c in r.ck if c["kind"] == "rc"]
        out.append("  rc chunks: %d (len median %s, t_done median %s); rc chunk motion total %.1f s vs rest-measure total %.1f s" % (
            len(rcn), fmt(med([c["len"] for c in rcn]), 0), fmt(med([c["t_done"] for c in rcn]), 0),
            sum(max(0, c["t_done"]) for c in rcn) / 1000.0, sum(rc_end_wall) / 1000.0))
        # rc passes per run
        per = [sum(1 for c in r.ck if c["kind"] == "rc") for r in runs
               if r.cls["landing"] == "recatch" and r.cls["type"] != "rtest_prep"]
        out.append("  rc chunks per recatch-landing run: median %s p90 %s max %s (runs=%d)" % (
            fmt(med(per), 0), fmt(pct(per, 90), 0), fmt(max(per), 0), len(per)))
    else:
        out.append("  no recatch runs / no rc chunks in these logs")
    return out


def sink_cells(runs):
    """Non-overlapping decomposition of every ms of wall into (bucket, comp) cells with a physical-minimum estimate."""
    cells = collections.OrderedDict()
    # reference minima derived from the data itself
    all_mv = [m for r in runs for m in r.mv if not m.get("first")]
    frames = [x for v in frame_intervals(runs).get("7", ([], []))[1:] for x in v]
    frame_ms = med(frames) or 42.0
    samp1 = med([m["samp"] for m in all_mv if m["tries"] == 1 and m["samp"] > 0]) or 10
    over = [m["over"] for m in all_mv if "over" in m and not m["after_ck"] and m["ph"] != "i" and m["nchunks"] == 1]
    over_min = max(0.0, pct(over, 10) or 0)
    sess = [r.bsum.get("session", 0) for r in runs]
    sess_min = pct(sess, 10) or 0
    epi_fast = [r.epilogue_ms for r in runs if r.epi_fast and r.epilogue_ms]
    epi_min = pct(epi_fast, 10) if epi_fast else 0
    ref = dict(frame_ms=frame_ms, samp1=samp1, over_min=over_min, sess_min=sess_min, epi_min=epi_min)

    def add(key, ms, saving):
        c = cells.setdefault(key, [0.0, 0.0])
        c[0] += ms
        c[1] += max(0.0, saving)

    for r in runs:
        for c in r.cells:
            b, comp, ms = c["bucket"], c["comp"], c["ms"]
            if comp == "session":
                add((b, "session open+LV recover"), ms, ms - sess_min)
            elif comp == "epilogue":
                add((b, "epilogue"), ms, ms - epi_min if r.epilogue_ms else 0)
            elif comp == "motion":
                model = model_ms(min(c["cmd"], 300 if c["spd"].lower() == "1f" else 100), c["spd"])
                add((b, "move completion wait (done_ms)"), ms, ms - model)
            elif comp == "wait":
                add((b, "settle wait= (before measure)"), ms, ms - frame_ms)
            elif comp == "samp":
                add((b, "measure samp="), ms, ms - samp1)
            elif comp == "overhead":
                add((b, "per-move dispatch overhead"), ms, ms - over_min)
            elif comp == "post_ck_overhead":
                add((b, "overhead after a tracked chunk (coast/pad/decide)"), ms, ms - over_min)
            elif comp == "multichunk":
                add((b, "earlier chunks of multi-chunk moves"), ms, ms - c["extra_model"])
            elif comp == "ck_motion":
                add((b, "chunk motion (t_done)"), ms, ms - c["model"])
            elif comp == "ck_gap":
                add((b, "chunk gap (t_send lag)"), ms, ms)
    return cells, ref


def section5(runs):
    out = ["", "=" * 100, "5. TOP TIME SINKS (non-overlapping cells of every wall-clock ms) AND SAVING AT THE PHYSICAL MINIMUM", "=" * 100]
    prep = [r for r in runs if r.cls["type"] == "rtest_prep"]
    runs = [r for r in runs if r.cls["type"] != "rtest_prep"]
    cells, ref = sink_cells(runs)
    total = sum(r.wall for r in runs)
    out.append("total wall over %d AF runs = %.1f s (the %d Refocus-test prep runs, %.1f s of test-harness defocus repositioning, "
               "are excluded)" % (len(runs), total / 1000.0, len(prep), sum(r.wall for r in prep) / 1000.0))
    out.append("physical minima used (derived from the logs): motion = %d ms latency + |pulses| x 2.9 ms (07) or 1.0 ms (1F), "
               "settle wait = 1 LV frame (%.0f ms), measure = single-try median samp (%.0f ms), dispatch overhead = p10 of "
               "all move overheads (%.0f ms), session = p10 session (%.0f ms), epilogue = p10 of fast epilogues (%.0f ms), "
               "chunk gap = 0." % (LAT_MS, ref["frame_ms"], ref["samp1"], ref["over_min"], ref["sess_min"], ref["epi_min"]))
    rows = sorted(cells.items(), key=lambda kv: -kv[1][0])
    out.append("")
    out.append("  %2s %-52s %9s %6s %10s %6s" % ("#", "bucket / component", "total_s", "share", "saving_s", "%wall"))
    for i, ((b, comp), (ms, sv)) in enumerate(rows[:8], 1):
        out.append("  %2d %-52s %9.1f %5.1f%% %10.1f %5.1f%%" % (i, "%s: %s" % (b, comp), ms / 1000.0, 100 * ms / total,
                                                                 sv / 1000.0, 100 * sv / total))
    out.append("  -- remaining cells --")
    for i, ((b, comp), (ms, sv)) in enumerate(rows[8:], 9):
        out.append("  %2d %-52s %9.1f %5.1f%% %10.1f %5.1f%%" % (i, "%s: %s" % (b, comp), ms / 1000.0, 100 * ms / total,
                                                                 sv / 1000.0, 100 * sv / total))
    chk = sum(v[0] for v in cells.values())
    out.append("  cells sum to %.1f s (check: total wall %.1f s)" % (chk / 1000.0, total / 1000.0))
    # overlapping extra: local fallback waste
    real = [r for r in runs if r.cls["type"] != "rtest_prep"]
    fb = [r for r in real if r.local_fallback]
    cost = sum(r.bsum.get("local", 0) for r in fb)
    keep = 0
    for r in fb:
        lm = [m["wall"] for m in r.mv if m["bucket"] == "local"][:2]   # the two +/- probes needed to decide
        keep += sum(lm)
    out.append("")
    out.append("  overlapping extra (not a separate cell): Local attempts that ended in the B fallback cost %.1f s in %d runs; "
               "if the fallback could be decided after the first two probes, about %.1f s would be saved." %
               (cost / 1000.0, len(fb), max(0, cost - keep) / 1000.0))
    # whole-bucket view
    out.append("")
    out.append("  bucket totals (all runs): " + "; ".join(
        "%s %.1fs (%.0f%%)" % (b, sum(r.bsum.get(b, 0) for r in runs) / 1000.0,
                               100.0 * sum(r.bsum.get(b, 0) for r in runs) / total) for b in BUCKETS))
    return out


# ---------------------------------------------------------------- main
def analyse(dirs, since_header=None):
    runs, meta = load(dirs, since_header)
    meta["rt_matched"] = classify(runs, meta["rt_runs"])
    for r in runs:
        attribute(r)
    return runs, meta


def report(runs, meta, sections=(1, 2, 3, 4, 5)):
    out = []
    out.append("AF time budget: %d unique runs (%d duplicate copies dropped, %d same-header/different-result kept)" %
               (len(runs), meta["dups"], meta["conflicts"]))
    inc = [r for r in runs if r.result is None]
    out.append("  runs without a RESULT line (truncated, excluded from the tables): %d" % len(inc))
    out.append("  RT_RUN lines in AFRTEST.LOG: %d unique, matched to AFDRIVE runs by (land_s, moves): %d" %
               (len(meta["rt_runs"]), meta["rt_matched"]))
    issues = collections.Counter(i.split(" at")[0].split(" before")[0] for r in runs for i in r.issues)
    unk = collections.Counter()
    for r in runs:
        unk.update(r.notes)
    out.append("  parsing caveats: %s; %s" % (dict(issues) or "no clock anomalies", dict(unk) or "no unknown phase letters"))
    use = [r for r in runs if r.result is not None]
    if 1 in sections:
        out += section1(use)
    if 2 in sections:
        out += section2(use)
    if 3 in sections:
        out += section3(use)
    if 4 in sections:
        out += section4(use)
    if 5 in sections:
        out += section5(use)
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--logs", nargs="+", required=True, help="session folder(s), oldest first (ML/LOGS inside is found)")
    ap.add_argument("--since-header", help="only runs from the first AFDRIVE header containing this text onward "
                                           "(per log file), e.g. 't=22339 lens'")
    ap.add_argument("--section", default="1,2,3,4,5", help="comma list of sections to print")
    a = ap.parse_args(argv)
    dirs = sorted(a.logs)
    runs, meta = analyse(dirs, a.since_header)
    if not runs:
        print("no runs found", file=sys.stderr)
        return 2
    print(report(runs, meta, tuple(int(x) for x in a.section.split(","))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
