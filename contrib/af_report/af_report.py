#!/usr/bin/env python3
"""Summarise the EOS M ML AF logs (AFDRIVE/AFBENCH/AFRTEST/CRASH) into a short report."""
import argparse, glob, json, os, re, shutil, statistics as st, subprocess, sys, time

DEF_CARD = "/Volumes/EOS_DIGITAL"
STATE = os.path.expanduser("~/.cache/af_report/state.json")
SESSION_GAP = 2 * 3600  # files within this of the newest mtime form "the newest session"


def kv(line):
    return {k: v for k, v in re.findall(r"(\w+)=(\S+)", line)}


def num(d, k, default=None):
    try:
        return float(d[k].split("/")[0])
    except (KeyError, ValueError):
        return default


def med(xs):
    return st.median(xs) if xs else None


def p90(xs):
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(0.9 * (len(s) - 1))))]


def fmt(x, nd=0, unit=""):
    return "-" if x is None else ("%.*f%s" % (nd, x, unit))


def ts(t):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))


def parse_since(s):
    m = re.fullmatch(r"(\d+(?:\.\d+)?)([hmd])", s)
    if m:
        return time.time() - float(m.group(1)) * {"h": 3600, "m": 60, "d": 86400}[m.group(2)]
    for f in ("%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return time.mktime(time.strptime(s, f))
        except ValueError:
            pass
    sys.exit("bad --since (use '2h', '30m', '2d', 'YYYY-MM-DD[ HH:MM]')")


def lines_of(path):
    try:
        with open(path, "r", errors="replace") as f:
            return [l.rstrip("\r\n") for l in f]
    except OSError:
        return []


def find_dirs(args):
    """Return (log_dirs, root_dirs) to scan."""
    if args.logs:
        base = args.logs
    else:
        base = args.card
        if not os.path.isdir(base):
            r = subprocess.run(["diskutil", "mount", "/dev/disk4s1"], capture_output=True, text=True)
            print("card not mounted at %s; ran 'diskutil mount /dev/disk4s1': %s"
                  % (base, (r.stdout + r.stderr).strip().splitlines()[-1:] or r.returncode))
            if not os.path.isdir(base):
                sys.exit("card still not available")
        if args.copy:
            dst = os.path.join(args.copy, time.strftime("%Y%m%d-%H%M%S"))
            os.makedirs(dst)
            for pat in ("ML/LOGS/*.LOG", "*.LOG"):
                for p in glob.glob(os.path.join(base, pat)):
                    d = os.path.join(dst, os.path.relpath(os.path.dirname(p), base))
                    os.makedirs(d, exist_ok=True)
                    shutil.copy2(p, d)
            print("copied logs to %s" % dst)
            base = dst
    dirs = [base]
    if os.path.isdir(os.path.join(base, "ML", "LOGS")):
        dirs.append(os.path.join(base, "ML", "LOGS"))
    return dirs


def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(key, t):
    try:
        s = load_state()
        s[key] = t
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        with open(STATE, "w") as f:
            json.dump(s, f)
    except OSError:
        pass


# ---------------------------------------------------------------- parsing

def parse_drive(lines):
    runs, cur = [], None
    notes = []  # (kind, dict-or-line)
    seg_events = [[]]  # per boot segment: list of (t, kind)
    focal = set()
    last_t = -1
    for l in lines:
        m = re.search(r"\bt=(\d+)", l)
        if l.startswith("# AFDRIVE"):
            cur = {"algo": kv(l).get("algo"), "focal": None}
        if m and re.match(r"# (aref |lv o|monitor )", l):
            t = int(m.group(1))
            if t < last_t:
                seg_events.append([])
            last_t = t
            kind = None
            if l.startswith("# aref trig"):
                kind = "trig"
            elif l.startswith("# lv off"):
                kind = "off"
            elif l.startswith("# lv on"):
                kind = "on"
            seg_events[-1].append((t, kind))
        if l.startswith("# focal_mm="):
            d = kv(l)
            focal.add(d.get("focal_mm"))
            if cur is not None:
                cur["focal"] = d.get("focal_mm")
        elif l.startswith("# RESULT"):
            d = kv(l)
            d["_focal"] = cur and cur["focal"]
            runs.append(d)
        elif l.startswith("# aref "):
            parts = l.split()
            if len(parts) > 2:          # a truncated line at the end of a log must not crash the report
                notes.append((parts[2], kv(l)))
        elif l.startswith("# endstop"):
            notes.append(("endstop", {}))
        elif l.startswith("# REC ") or l.startswith("REC "):
            notes.append(("REC" if l.startswith("REC ") else "RECnote", kv(l)))
    return runs, notes, seg_events, focal


def lv_minutes(seg_events):
    """LV time per boot segment: span of event times minus off->on gaps."""
    total = 0.0
    for ev in seg_events:
        if len(ev) < 2:
            continue
        span = ev[-1][0] - ev[0][0]
        off_t = None
        for t, k in ev:
            if k == "off" and off_t is None:
                off_t = t
            elif k == "on" and off_t is not None:
                span -= t - off_t
                off_t = None
        total += max(span, 0)
    return total / 60000.0


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--card", default=DEF_CARD, help="card mount point (default %s)" % DEF_CARD)
    ap.add_argument("--logs", help="read a copied folder instead of the card")
    ap.add_argument("--since", help="only logs modified since: 2h, 30m, 2d, 'YYYY-MM-DD HH:MM'")
    ap.add_argument("--all", action="store_true", help="do not filter by session")
    ap.add_argument("--copy", metavar="DIR", help="copy the logs into DIR/<timestamp> first")
    ap.add_argument("--no-state", action="store_true", help="do not read/update the last-run time")
    args = ap.parse_args()

    dirs = find_dirs(args)
    files = {}
    for d in dirs:
        for p in glob.glob(os.path.join(d, "*.LOG")):
            files[p] = os.stat(p).st_mtime
    if not files:
        print("no *.LOG files found in %s (nothing to report)" % ", ".join(dirs))
        return

    newest = max(files.values())
    if args.all:
        since = 0
    elif args.since:
        since = parse_since(args.since)
    else:
        since = newest - SESSION_GAP
    key = os.path.abspath(args.logs or args.card)
    prev_run = None if args.no_state else load_state().get(key)

    out = []
    P = out.append
    verdict = []
    by_name = {os.path.basename(p): p for p in files}  # ML/LOGS later in glob order wins
    inses = {n: p for n, p in by_name.items() if files[p] >= since}

    # 1. card
    names = sorted(by_name)
    P("== Card: %s  (newest log %s, session since %s)" % (key, ts(newest), "start" if args.all else ts(since)))
    af = [n for n in names if re.match(r"AF(DRIVE|BENCH|RTEST|REV|STOP|SWEEP)\.LOG$", n)]
    P("   AF logs: " + (", ".join("%s%s" % (n, "*" if n in inses else "") for n in af) or "none")
      + "   (* = modified in window)")
    other = len([n for n in names if n not in af and not re.match(r"(CRASH|ASSERT)", n)])
    if other:
        P("   %d other logs (AFDIAG/AFCAL/...) not analysed" % other)

    # 2. crashes
    crashes = [p for p in files if re.match(r"(CRASH|ASSERT)\d*\.LOG$", os.path.basename(p))]
    lim = prev_run if prev_run else since
    if args.all:
        lim = 0
    new_cr = sorted((p for p in crashes if files[p] > lim), key=lambda p: files[p])
    if new_cr:
        P("")
        P("== Crashes: %d new (since %s) of %d total" % (len(new_cr), ts(lim) if lim else "ever", len(crashes)))
        for p in new_cr[-6:]:
            P("   %s (%s)" % (os.path.basename(p), ts(files[p])))
            for l in lines_of(p)[:3]:
                P("      " + l[:110])
        verdict.append("CRASH: %d new crash/assert log(s)" % len(new_cr))
    elif crashes:
        P("")
        P("== Crashes: none new (%d old crash/assert logs on card)" % len(crashes))

    # 3/4/7. drive
    runs = notes = seg = focal = None
    if "AFDRIVE.LOG" in inses:
        runs, notes, seg, focal = parse_drive(lines_of(inses["AFDRIVE.LOG"]))
    if runs:
        ok = [r for r in runs if r.get("aborted") == "0"]
        rate = 100.0 * len(ok) / len(runs)
        pcts = [num(r, "land_pct") for r in ok if num(r, "land_pct") is not None]
        tms = [num(r, "total_ms") for r in runs if num(r, "total_ms") is not None]
        tok = [num(r, "total_ms") for r in ok if num(r, "total_ms") is not None]
        hist = {}
        for r in runs:
            a = r.get("aborted", "?")
            if a != "0":
                hist[a] = hist.get(a, 0) + 1
        algos = {}
        for r in runs:
            algos[r.get("algo", "?")] = algos.get(r.get("algo", "?"), 0) + 1
        endstops = sum(1 for k, _ in notes if k == "endstop")
        P("")
        P("== AF drive runs: %d, success %d (%.0f%%)" % (len(runs), len(ok), rate))
        P("   land_pct med %s (ok runs), total_ms med %s / p90 %s (all), ok-only med %s"
          % (fmt(med(pcts)), fmt(med(tms)), fmt(p90(tms)), fmt(med(tok))))
        P("   aborts: %s%s" % (", ".join("%s=%d" % kvp for kvp in sorted(hist.items(), key=lambda x: -x[1])) or "none",
                               "   endstops: %d" % endstops if endstops else ""))
        P("   algo: %s   focal_mm: %s" % (", ".join("%s x%d" % kvp for kvp in sorted(algos.items())),
                                         ", ".join(sorted(f for f in focal if f)) or "?"))
        med_s = med(tok) if tok else med(tms)
        if rate >= 90 and med_s is not None and med_s <= 5000:
            verdict.append("AF drive OK (%.0f%% success, median %.1f s)" % (rate, med_s / 1000))
        else:
            verdict.append("AF drive NOT OK (%.0f%% success, median %s s; need >=90%% and <=5 s)"
                           % (rate, fmt(med_s and med_s / 1000, 1)))
        if hist.get("lowtexture"):
            verdict.append("lowtexture aborts: %d" % hist["lowtexture"])

    if notes:
        trigs = [d for k, d in notes if k == "trig"]
        dones = [d for k, d in notes if k == "done"]
        holds = sum(1 for k, _ in notes if k == "hold")
        skips = sum(1 for k, _ in notes if k == "skip")
        if trigs or dones or holds or skips:
            gaps = []
            for ev in seg:
                tt = [t for t, k in ev if k == "trig"]
                gaps += [(b - a) / 1000.0 for a, b in zip(tt, tt[1:]) if b > a]
            lvm = lv_minutes(seg)
            hunt = len(trigs) / lvm if lvm > 0.2 else None
            dok = sum(1 for d in dones if (num(d, "pct", -1) >= 70))
            P("")
            P("== Auto refocus: %d trig, %d done (%d ok pct>=70, %d fail), %d hold, %d skip"
              % (len(trigs), len(dones), dok, len(dones) - dok, holds, skips))
            P("   median gap between trigs %s s; LV time %.1f min; hunting index %s trig/min"
              % (fmt(med(gaps), 0), lvm, fmt(hunt, 2)))
            pairs, pend = [], None
            for k, d in notes:
                if k == "trig":
                    pend = d
                elif k == "done" and pend is not None:
                    pairs.append((pend, d))
                    pend = None
            for a, b in pairs[-10:]:
                P("   t=%5.0fs %3s%% -> %4s%%  %5.1fs %s mm=%s"
                  % (num(a, "t", 0) / 1000, a.get("pct", "?"), b.get("pct", "?"),
                     num(b, "ms", 0) / 1000, b.get("path", "?"), b.get("mm", "?")))
            if hunt is not None and hunt > 2:
                verdict.append("Auto refocus HUNTING (index %.1f/min > 2)" % hunt)
            elif trigs:
                verdict.append("Auto refocus trigger rate OK (%s/min)" % fmt(hunt, 2))

    # 5. bench
    if "AFBENCH.LOG" in inses:
        bl = lines_of(inses["AFBENCH.LOG"])
        rows = {}
        for l in bl:
            if l.startswith("# BENCH algo="):
                d = kv(l)
                rows.setdefault(d.get("algo"), {}).setdefault(d.get("defocus"), []).append(d)
        summ = [kv(l) for l in bl if l.startswith("# BENCH_SUMMARY")]
        if rows:
            defs = sorted({x for a in rows.values() for x in a}, key=lambda x: float(x))
            P("")
            P("== Bench (land pct / ms, median):")
            P("   %-6s" % "algo" + "".join("%12s" % d for d in defs) + "   summary")
            for a in sorted(rows):
                cells = []
                for d in defs:
                    rs = rows[a].get(d, [])
                    cells.append("%12s" % ("-" if not rs else "%s/%s%s" % (
                        fmt(med([num(r, "pct", 0) for r in rs])), fmt(med([num(r, "ms", 0) for r in rs])),
                        "!" if any(r.get("ab", "0") != "0" for r in rs) else "")))
                s = [x for x in summ if x.get("algo") == a]
                P("   %-6s" % a + "".join(cells) + ("   " + " ".join("n=%s ok=%s" % (x.get("n"), x.get("ok"))
                                                                    for x in s[-1:]) if s else ""))
            P("   (! = some aborted)")

    # 6. refocus test.  Patch 2 appends notrig/valid/false_pass to # RTEST and
    # no_trigger/valid/false_pass to # RT_TRIAL, so trial lines must be shown in full
    # (240 chars; the old 140-char cut hid the trailing integrity fields).
    if "AFRTEST.LOG" in inses:
        rl = [l for l in lines_of(inses["AFRTEST.LOG"]) if l.startswith(("# RTEST", "# RT_TRIAL"))]
        summ = [l for l in rl if re.match(r"# RTEST\s", l)]
        marker = [l for l in rl if re.match(r"# RTEST_(BEGIN|END)", l)]
        trial = [l for l in rl if l.startswith("# RT_TRIAL ")]
        invalid = [l for l in rl if l.startswith("# RT_TRIAL_INVALID")]
        maxline = 240
        if rl:
            P("")
            extra = " (%d invalid)" % len(invalid) if invalid else ""
            P("== Refocus test (AFRTEST): %d summary, %d trial lines%s"
              % (len(summ), len(trial) + len(invalid), extra))
            d = kv(summ[-1]) if summ else {}
            m = re.search(r"ok=(\d+)/(\d+)", summ[-1]) if summ else None
            of = int(m.group(2)) if m else None
            notrig = num(d, "notrig")
            valid = num(d, "valid")
            false_pass = num(d, "false_pass")
            ft = num(d, "false_trig")
            for l in summ[-3:]:
                P("   " + l[2:][:maxline])
            if notrig is not None or valid is not None or false_pass is not None:
                # modern (patch 2+) log: surface the integrity counters explicitly
                P("   integrity: notrig=%s valid=%s/%s false_pass=%s"
                  % (fmt(notrig), fmt(valid), fmt(of), fmt(false_pass)))
            for l in marker:
                P("   " + l[2:][:maxline])
            for l in trial[:12]:
                P("   " + l[2:][:maxline])
            if len(trial) > 12:
                P("   ... %d more trial lines" % (len(trial) - 12))
            for l in invalid[:6]:
                P("   " + l[2:][:maxline])
            if len(invalid) > 6:
                P("   ... %d more invalid-trial lines" % (len(invalid) - 6))
            if ft is not None and ft > 1:
                verdict.append("Auto refocus HUNTING (RTEST false_trig=%d)" % ft)
            if m and int(m.group(1)) < int(m.group(2)):
                verdict.append("Refocus test: only %s/%s ok" % m.groups())
            if notrig is not None and notrig > 0:
                verdict.append("Refocus test: %d trial(s) did not trigger (invalid; valid=%s/%s)"
                               % (int(notrig), fmt(valid), fmt(of)))
            if false_pass is not None and false_pass > 0:
                verdict.append("Refocus test: %d false pass(es) (ok counted on a no-trigger trial)"
                               % int(false_pass))

    # 7. recording
    if notes:
        recs = [d for k, d in notes if k == "REC"]
        rnotes = [d for k, d in notes if k == "RECnote"]
        arec = [d for k, d in notes if k == "rec"]
        if recs or arec or rnotes:
            P("")
            P("== Recording: %d REC lines, %d '# aref rec' notes" % (len(recs), len(arec)))
            for d in recs[-6:]:
                gaps = "/".join(d.get(k, "?") for k in ("vs_base_maxgap", "vs_run_maxgap", "vs_post_maxgap"))
                P("   stopped_in_run=%s land_pct=%s ms=%s aborted=%s vsync maxgap base/run/post=%s"
                  % (d.get("stopped_in_run"), d.get("land_pct"), d.get("ms"), d.get("aborted"), gaps))
                if d.get("stopped_in_run") not in (None, "0"):
                    verdict.append("Recording STOPPED during AF run")
            if arec:
                P("   aref rec: " + ", ".join("%s%%/%.0fs" % (d.get("pct"), num(d, "ms", 0) / 1000) for d in arec[-10:]))

    P("")
    P("== Verdict")
    if not verdict:
        P("   no data in window (try --all or --since)")
    for v in verdict:
        P("   - " + v)

    print("\n".join(out))
    if not args.no_state:
        save_state(key, time.time())


if __name__ == "__main__":
    main()
