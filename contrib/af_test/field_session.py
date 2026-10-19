#!/usr/bin/env python3
"""One-command checker for an EOS M ML AF field session.

Reads the AF logs from the card (or a copied folder), prints the verdicts that matter for the
patches 1-5 test, and optionally copies the session off the card first.

    python3 contrib/af_test/field_session.py                 # card at /Volumes/EOS_DIGITAL
    python3 contrib/af_test/field_session.py --copy          # copy to ~/af_logs/af-<timestamp>, then check
    python3 contrib/af_test/field_session.py --logs DIR      # a copied folder (may contain ML/LOGS)
    python3 contrib/af_test/field_session.py --all           # do not filter to the newest session

Stdlib only. Exit 0 = all available checks pass, 1 = a hard failure, 2 = incomplete/inconclusive.
The authoritative detailed report is still contrib/af_report/af_report.py and the stop traces
contrib/af_test/stop_analysis.py; this script is the fast acceptance check after a session.
"""
import argparse
import glob
import os
import re
import shutil
import statistics as st
import sys
import time

CARD = "/Volumes/EOS_DIGITAL"
SESSION_GAP = 2 * 3600.0


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


def newest_matching(logs_dir, patterns):
    best = None
    for pat in patterns:
        for p in glob.glob(os.path.join(logs_dir, pat)):
            m = os.stat(p).st_mtime
            if best is None or m > best[0]:
                best = (m, p)
    return best[1] if best else None


def in_window(path, since):
    return path and os.stat(path).st_mtime >= since


def fmt_pct(x):
    return "n/a" if x is None else "%d%%" % x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", help="copied log folder instead of the card")
    ap.add_argument("--card", default=CARD)
    ap.add_argument("--copy", action="store_true", help="copy the session to ~/af_logs/af-<timestamp>")
    ap.add_argument("--all", action="store_true", help="do not filter to the newest session")
    ap.add_argument("--dest", default=os.path.expanduser("~/af_logs"),
                    help="parent directory for --copy (default ~/af_logs)")
    args = ap.parse_args()

    base = args.logs or args.card
    if not os.path.isdir(base):
        print("log folder not found: %s" % base)
        print("mount the card (diskutil mount /dev/disk4s1) or pass --logs DIR")
        return 2

    if args.copy:
        dst = os.path.join(args.dest, time.strftime("af-%Y%m%d-%H%M%S"))
        n = 0
        for pat in ("ML/LOGS/*.LOG", "*.LOG"):
            for p in glob.glob(os.path.join(base, pat)):
                rel = os.path.relpath(os.path.dirname(p), base)
                d = os.path.join(dst, rel) if rel != "." else dst
                os.makedirs(d, exist_ok=True)
                shutil.copy2(p, d)
                n += 1
        print("copied %d log(s) to %s\n" % (n, dst))
        base = dst

    logs = find_logs_dir(base)
    files = sorted(glob.glob(os.path.join(logs, "*.LOG")), key=os.stat)
    if not files:
        print("no *.LOG files in %s" % logs)
        return 2
    newest = max(os.stat(p).st_mtime for p in files)
    since = 0 if args.all else newest - SESSION_GAP

    drive = newest_matching(logs, ["AFDRIVE.LOG"])
    rtest = newest_matching(logs, ["AFRTEST.LOG"])
    stop = newest_matching(logs, ["AFSTOP.LOG"]) or newest_matching(logs, ["AFSTR*.LOG"])
    bench = newest_matching(logs, ["AFBENCH.LOG"])
    sweep = newest_matching(logs, ["AFSWEEP.LOG"]) or newest_matching(logs, ["AFSW*.LOG"])
    afdiag = newest_matching(logs, ["AFDIAG.LOG"])

    # fall back to rotated names for any role that has no exact-name file in the window
    def usable(p):
        return p if p and (args.all or in_window(p, since)) else None

    drive, rtest, stop, bench, sweep, afdiag = map(usable, (drive, rtest, stop, bench, sweep, afdiag))
    present = [p for p in (drive, rtest, stop, bench, sweep) if p]
    if not present:
        print("no AF session logs newer than %s in %s" % (time.ctime(since), logs))
        print("(use --all to analyse whatever is there)")
        return 2

    problems, warnings, notes = [], [], []
    print("AF field session check")
    print("  source: %s" % base)
    print("  logs:   %s" % logs)
    print("  window: %s" % ("start" if args.all else time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(since))))
    for label, p in (("AFDRIVE", drive), ("AFRTEST", rtest), ("AFSTOP", stop),
                     ("AFBENCH", bench), ("AFSWEEP", sweep)):
        print("  %-8s %s" % (label, os.path.basename(p) if p else "-"))
    print()

    # ---------------------------------------------------------------- Refocus test
    print("== Refocus test")
    if not rtest:
        print("   not run (no AFRTEST.LOG)")
        notes.append("Refocus test not run")
    else:
        lines = read_lines(rtest)
        summary = [l for l in lines if l.startswith("# RTEST ")]
        trials = [l for l in lines if l.startswith("# RT_TRIAL ") and not l.startswith("# RT_TRIAL_INVALID")]
        invalid = [l for l in lines if l.startswith("# RT_TRIAL_INVALID")]
        if not summary:
            print("   no # RTEST summary (aborted before the end?)")
            notes.append("Refocus test has no summary")
        else:
            s = summary[-1]
            kv = parse_kv(s)
            modern = "notrig" in kv
            ok = int(kv.get("ok", "0").split("/")[0]) if kv.get("ok") else None
            notrig = int(kv["notrig"]) if modern else None
            valid = kv.get("valid")
            fpass = int(kv["false_pass"]) if "false_pass" in kv else None
            print("   " + s[2:])
            if modern:
                print("   integrity: notrig=%s valid=%s false_pass=%s" % (notrig, valid, fpass))
            else:
                print("   integrity: not present (pre-patch-2 build) - ok is not trustworthy "
                      "because no-trigger trials used to count")
            for l in trials[-5:]:
                print("   " + l[2:])
            for l in invalid[-5:]:
                print("   " + l[2:])
            if modern:
                if notrig and notrig > 0:
                    problems.append("Refocus test: %d trial(s) never triggered (invalid)" % notrig)
                if fpass:
                    problems.append("Refocus test: %d false pass(es)" % fpass)
                if valid and valid.split("/")[0] != valid.split("/")[-1]:
                    warnings.append("Refocus test: valid=%s (a trial did not trigger)" % valid)
                if ok is not None and ok < 4:
                    problems.append("Refocus test: only %s ok (target >= 4/5)" % kv.get("ok"))
                elif ok is not None and ok < 5:
                    warnings.append("Refocus test: %s ok (target 5/5)" % kv.get("ok"))
            else:
                notes.append("Refocus verdict is only comparable with the pre-patch baseline (2/5 anchored)")
            if kv.get("false_trig") not in (None, "0"):
                problems.append("Refocus test: false_trig=%s (target 0)" % kv.get("false_trig"))
        if drive:
            dl = read_lines(drive)
            soft = [l for l in dl if "climb still soft" in l or "wide still flat" in l]
            anchors = [l for l in dl if "ref anchor" in l or "ref max reset confirmed soft" in l]
            refmax = [l for l in dl if l.startswith("# refmax")]
            if soft:
                print("   local fallbacks: %d line(s), last: %s" % (len(soft), soft[-1][:140]))
            if refmax:
                print("   " + refmax[-1][:160])
            if anchors:
                print("   anchor lines (last 6):")
                for l in anchors[-6:]:
                    print("     " + l[:160])
                # a raise immediately after a confirmed-soft reset is the review-2 MF1 signature
                for i, l in enumerate(dl):
                    if "confirmed soft scene -> reset" in l:
                        tail = [x for x in dl[i + 1:i + 6] if "ref anchor" in x]
                        if tail and "raise" in tail[0]:
                            problems.append("reference: anchor raised right after a confirmed-soft reset")
                            break
    print()

    # ---------------------------------------------------------------- auto-refocus gates
    if afdiag:
        print("== Auto-refocus decisions (AFDIAG)")
        recs = []
        for line in read_lines(afdiag):
            p = line.split()
            if len(p) < 3 or p[1] != "@REFOCUS":
                continue
            mm = re.search(r"w=([0-9a-f,]+)", line)
            if not mm:
                continue
            w = [int(x, 16) for x in mm.group(1).split(",")] + [0] * 4
            recs.append(w[:4])
        gates = {}
        names = {0: "fired", 1: "not low", 2: "dwell", 3: "cooldown", 4: "no usable peak",
                 5: "unsteady", 6: "paused", 8: "history", 9: "settled", 10: "texture",
                 11: "failed scene"}
        runs = []
        triggers = 0
        for w in recs:
            k = w[0] & 0xff
            if k == 10:
                g = (w[0] >> 16) & 0xff
                gates[g] = gates.get(g, 0) + 1
            elif k == 13:
                pct = w[1] if w[1] != 0xFFFFFFFF else -1
                runs.append((pct, w[2]))
            elif k == 1:
                triggers += 1
        if gates:
            top = sorted(gates.items(), key=lambda kv: -kv[1])
            print("   gates: " + ", ".join("%s=%d" % (names.get(g, "gate%d" % g), n) for g, n in top))
        print("   triggers=%d runs=%d (failed=%d)" % (triggers, len(runs), sum(1 for p, _ in runs if p < 0)))
        for pct, ms in runs[-5:]:
            print("   run pct=%s ms=%d" % (pct, ms))
        if gates.get(4, 0) > 20:
            warnings.append("Auto refocus: %d 'no usable peak' decisions - trigger reference could not form"
                            % gates.get(4, 0))
        print()

    # ---------------------------------------------------------------- stop test
    print("== AF stop test")
    if not stop:
        print("   not run (no AFSTOP.LOG)")
        notes.append("AF stop test not run")
    else:
        sl = read_lines(stop)
        checks = [l for l in sl if l.startswith("# stop_check")]
        if checks:
            halted = [l for l in checks if parse_kv(l).get("halted") == "1"]
            for l in checks:
                print("   " + l[2:])
            print("   verdict: %d/%d halted (expected 0: (9,3,1) does not halt a (9,1) move)"
                  % (len(halted), len(checks)))
            if halted:
                warnings.append("AF stop test: %d/%d trial(s) show halted=1 - new evidence, revisit C/G"
                                % (len(halted), len(checks)))
        else:
            plans = [l for l in sl if l.startswith("# stop_plan")]
            print("   no # stop_check lines (pre-patch-2 build); %d stop_plan block(s) - use "
                  "contrib/af_test/stop_analysis.py on this file" % len(plans))
            notes.append("stop test uses the old log format")
    print()

    # ---------------------------------------------------------------- timings
    if drive:
        print("== AF drive timings")
        res = [l for l in read_lines(drive) if l.startswith("# RESULT ")]
        if res:
            for l in res[-6:]:
                kv = parse_kv(l)
                print("   algo=%s total_ms=%s search_ms=%s epilogue_ms=%s moves=%s land_pct=%s"
                      % (kv.get("algo", "?"), kv.get("total_ms", "?"), kv.get("search_ms", "-"),
                         kv.get("epilogue_ms", "-"), kv.get("moves", "?"), kv.get("land_pct", "?")))
            modern = [l for l in res if "search_ms=" in l]
            if not modern:
                notes.append("no search_ms/epilogue_ms (old build)")
        print()

    # ---------------------------------------------------------------- bench
    if bench:
        print("== AF bench")
        rows = [parse_kv(l) for l in read_lines(bench) if l.startswith("# BENCH algo=")]
        if rows:
            by = {}
            for r in rows:
                a = r.get("algo", "?")
                by.setdefault(a, []).append(r)
            for a in sorted(by):
                rs = by[a]
                ok = [r for r in rs if int(r.get("pct", "0")) >= 90]
                pcts = [int(r.get("pct", "0")) for r in rs]
                ms = [int(r.get("ms", "0")) for r in rs]
                print("   %-4s n=%d ok=%d/%d median_pct=%d median_ms=%d"
                      % (a, len(rs), len(ok), len(rs), st.median(pcts), st.median(ms)))
        print()

    # ---------------------------------------------------------------- sweep
    if sweep:
        sl = read_lines(sweep)
        sweeps = [l for l in sl if l.startswith("# sweep ")]
        done = [l for l in sl if l.startswith("# done ")]
        print("== AF sweep record: %d sweep block(s), %d done line(s)" % (len(sweeps), len(done)))
        for l in done[:8]:
            print("   " + l[2:])
        print()

    # ---------------------------------------------------------------- crashes
    crash_files = sorted(glob.glob(os.path.join(os.path.dirname(logs), "ASSERT*.LOG"))
                         + glob.glob(os.path.join(os.path.dirname(logs), "CRASH*.LOG"))
                         + glob.glob(os.path.join(logs, "ASSERT*.LOG"))
                         + glob.glob(os.path.join(logs, "CRASH*.LOG")))
    new_crash = [p for p in crash_files if os.stat(p).st_mtime >= since]
    print("== Crashes: %d new in the session window" % len(new_crash))
    for p in new_crash[-5:]:
        print("   " + os.path.basename(p))
        if p.endswith(".LOG"):
            problems.append("crash log: %s" % os.path.basename(p))
    print()

    # ---------------------------------------------------------------- verdict
    print("== Verdict")
    if problems:
        for x in problems:
            print("   FAIL  " + x)
    for x in warnings:
        print("   WARN  " + x)
    for x in notes:
        print("   note  " + x)
    if not problems and not warnings:
        print("   PASS  all available checks passed")
    elif not problems:
        print("   PASS with warnings")
    print()
    print("Next: keep this folder and send the output. Add the session to the regression corpus with")
    print("  python3 contrib/af_test/add_case.py --help")
    return 1 if problems else (0 if present else 2)


if __name__ == "__main__":
    sys.exit(main())
