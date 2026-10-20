#!/usr/bin/env python3
"""Shared AF lens profile database (patch 15).

The camera measures the focus-peak width of every lens / zoom once ("# PROFILE", stored on the card in
ML/SETTINGS/AFLENS.TXT).  This tool merges the profiles of many cards into the repo file
data/af_lens_profiles.txt and generates the table compiled into the module (modules/crop_rec/af_lens_db.h).

    python3 contrib/af_test/lens_db.py import [--src TAG] AFLENS.TXT|card-dir ...   # merge into the repo file
    python3 contrib/af_test/lens_db.py list                                          # print the repo file
    python3 contrib/af_test/lens_db.py export-header                                 # write af_lens_db.h

File format (one profile per line, '#' comments allowed):
    v1|name|mm|zoom|W50|floor_pct|done_word_hex|honest_pct|fine[|n=<samples>][|src=<tag,tag>][|w=<W50 samples>][|f=<floor samples>]
The first nine fields are the card format (AFLENS.TXT); n=, src=, w= and f= exist only in the repo file (w= / f= keep the
last 32 raw samples so that a later import takes the exact median instead of a median of medians).
Key = (name, mm, zoom).  Merging: same key -> median W50 / floor_pct / honest_pct (weighted by n), most common done word,
fine = clamp(round(0.7 W50), 6, 30), n summed.  A sample whose W50 is more than 2x (or less than 1/2) of the median of
what is already known for the key is an outlier: it is reported and NOT merged.

Stdlib only.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_DB = os.path.join(REPO, "data", "af_lens_profiles.txt")
DEFAULT_HEADER = os.path.join(REPO, "modules", "crop_rec", "af_lens_db.h")
HEADER_COMMENT = """\
# AF lens profiles shipped with the module (source of truth for modules/crop_rec/af_lens_db.h).
# Line format: v1|name|mm|zoom|W50|floor_pct|done_word_hex|honest_pct|fine|n=<samples>|src=<contributors>
# Same as the camera's ML/SETTINGS/AFLENS.TXT plus n= and src=.  Maintained by contrib/af_test/lens_db.py
# (import / list / export-header); see the "Sharing lens profiles" section of contrib/af_test/README.md.
"""
OUTLIER_X = 2.0
MAX_RAW = 32


def samples_of(e, field, value):
    """The raw samples behind an entry: the stored list, else its aggregate repeated n times."""
    lst = e.get(field) or []
    return list(lst) if lst else [value] * min(e["n"], MAX_RAW)


def median(vals):
    v = sorted(vals)
    return v[(len(v) - 1) // 2]


def fine_from_w50(w50):
    return max(6, min(30, (w50 * 7 + 5) // 10))


def parse_line(line):
    """One profile line -> dict, or None for comments / bad lines (tolerant, like the camera's parser)."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    f = line.split("|")
    if len(f) < 9 or f[0] != "v1" or len(f[1]) > 31:
        return None
    try:
        mm, zoom, w50, floor_pct = int(f[2]), int(f[3]), int(f[4]), int(f[5])
        done = int(f[6], 16)
        honest, fine = int(f[7]), int(f[8])
    except ValueError:
        return None
    if not (0 <= mm <= 400 and 1 <= zoom <= 10 and 1 <= w50 <= 400 and -1 <= floor_pct <= 300
            and 0 <= honest <= 100 and 6 <= fine <= 30):
        return None
    n, src, ws, fs = 1, [], [], []
    for extra in f[9:]:
        if extra.startswith("n="):
            try:
                n = max(1, int(extra[2:]))
            except ValueError:
                return None
        elif extra.startswith("src="):
            src = [s for s in extra[4:].split(",") if s]
        elif extra.startswith("w=") or extra.startswith("f="):
            try:
                vals = [int(x) for x in extra[2:].split(",") if x]
            except ValueError:
                return None
            if extra[0] == "w":
                ws = vals
            else:
                fs = vals
    return {"name": f[1], "mm": mm, "zoom": zoom, "w50": w50, "floor_pct": floor_pct, "done": done,
            "honest": honest, "fine": fine, "n": n, "src": src, "ws": ws, "fs": fs}


def format_line(e):
    line = "v1|%s|%d|%d|%d|%d|%x|%d|%d|n=%d|src=%s" % (
        e["name"], e["mm"], e["zoom"], e["w50"], e["floor_pct"], e["done"], e["honest"], e["fine"], e["n"],
        ",".join(e["src"]) if e["src"] else "unknown")
    if e.get("ws"):
        line += "|w=" + ",".join(str(x) for x in e["ws"][-MAX_RAW:])
    if e.get("fs"):
        line += "|f=" + ",".join(str(x) for x in e["fs"][-MAX_RAW:])
    return line


def key(e):
    return (e["name"], e["mm"], e["zoom"])


def read_db(path):
    db = {}
    try:
        with open(path, errors="replace") as f:
            for line in f:
                e = parse_line(line)
                if e:
                    db[key(e)] = e
    except OSError:
        pass
    return db


def write_db(path, db):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write(HEADER_COMMENT)
        for k in sorted(db):
            f.write(format_line(db[k]) + "\n")


def weighted_median(pairs):
    """pairs = [(value, weight)]; lower median of the expanded list."""
    items = sorted(pairs)
    total = sum(w for _, w in items)
    acc = 0
    for v, w in items:
        acc += w
        if acc * 2 >= total:
            return v
    return items[-1][0]


def merge(db, samples, src_tag):
    """Merge sample profiles into db (dict key -> entry).  Returns (merged, outliers) as lists of strings."""
    merged, outliers = [], []
    by_key = {}
    for s in samples:
        by_key.setdefault(key(s), []).append(s)
    for k, group in sorted(by_key.items()):
        old = db.get(k)
        # reference for the outlier test: the known median, else the median of this batch
        ref = old["w50"] if old else weighted_median([(s["w50"], s["n"]) for s in group])
        good = []
        for s in group:
            if s["w50"] > OUTLIER_X * ref or s["w50"] * OUTLIER_X < ref:
                outliers.append("%s mm=%d zoom=%d W50=%d (median %d)" % (k[0], k[1], k[2], s["w50"], ref))
            else:
                good.append(s)
        if not good:
            continue
        pool = ([old] if old else []) + good
        ws = (samples_of(old, "ws", old["w50"]) if old else [])
        fs = ([x for x in samples_of(old, "fs", old["floor_pct"]) if x >= 0] if old else [])
        for s in good:
            ws += samples_of(s, "ws", s["w50"])
            fs += [x for x in samples_of(s, "fs", s["floor_pct"]) if x >= 0]
        w50 = median(ws)
        floors = fs
        counts = {}
        for e in pool:
            counts[e["done"]] = counts.get(e["done"], 0) + e["n"]
        srcs = []
        for e in pool:
            for t in (e["src"] or []):
                if t not in srcs:
                    srcs.append(t)
        for s in good:
            if src_tag and src_tag not in srcs and not s["src"]:
                srcs.append(src_tag)
        if not srcs and src_tag:
            srcs.append(src_tag)
        db[k] = {"name": k[0], "mm": k[1], "zoom": k[2], "w50": w50,
                 "floor_pct": median(floors) if floors else -1, "ws": ws[-MAX_RAW:], "fs": floors[-MAX_RAW:],
                 "done": max(sorted(counts), key=lambda d: counts[d]),
                 "honest": weighted_median([(e["honest"], e["n"]) for e in pool]),
                 "fine": fine_from_w50(w50), "n": sum(e["n"] for e in pool), "src": srcs}
        merged.append("%s mm=%d zoom=%d W50=%d n=%d" % (k[0], k[1], k[2], w50, db[k]["n"]))
    return merged, outliers


def find_card_file(path):
    """AFLENS.TXT itself, or a card / copy folder that contains ML/SETTINGS/AFLENS.TXT (or SETTINGS/ or the file directly)."""
    if os.path.isfile(path):
        return path
    for rel in (("ML", "SETTINGS", "AFLENS.TXT"), ("SETTINGS", "AFLENS.TXT"), ("AFLENS.TXT",)):
        p = os.path.join(path, *rel)
        if os.path.isfile(p):
            return p
    return None


def cmd_import(args):
    db = read_db(args.db)
    samples = []
    for path in args.paths:
        f = find_card_file(path)
        if not f:
            print("no AFLENS.TXT in %s" % path, file=sys.stderr)
            return 2
        tag = args.src or os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(f))))) or "card"
        with open(f, errors="replace") as fh:
            for line in fh:
                e = parse_line(line)
                if e:
                    e["src"] = e["src"] or [args.src or tag]
                    samples.append(e)
    merged, outliers = merge(db, samples, args.src)
    write_db(args.db, db)
    for m in merged:
        print("merged   %s" % m)
    for o in outliers:
        print("OUTLIER  %s: not merged" % o)
    print("%d profile(s) merged, %d outlier(s), %d in %s" % (len(merged), len(outliers), len(db), args.db))
    return 1 if outliers else 0


def cmd_list(args):
    db = read_db(args.db)
    print("%-32s %4s %4s %5s %6s %5s %6s %4s %3s %s" % ("lens", "mm", "zoom", "W50", "floor%", "fine", "done", "hon%", "n", "src"))
    for k in sorted(db):
        e = db[k]
        print("%-32s %4d %4d %5d %6d %5d %6x %4d %3d %s" % (e["name"], e["mm"], e["zoom"], e["w50"], e["floor_pct"], e["fine"],
                                                           e["done"], e["honest"], e["n"], ",".join(e["src"])))
    print("%d profile(s)" % len(db))
    return 0


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header_text(db):
    out = ["/* generated by contrib/af_test/lens_db.py export-header from data/af_lens_profiles.txt - do not edit */"]
    keys = sorted(db)
    if not keys:
        out.append("/* 0 profiles: the table keeps one zeroed sentinel entry so that the array is never empty */")
    out.append("#define AFPR_BUILTIN_N %d" % len(keys))
    out.append("static const struct afpr_bi afpr_builtin[] = {")
    # struct afpr_bi { const char *name; short mm, zoom, w50, floor_pct, fine; } (defined in crop_rec.c)
    for k in keys:
        e = db[k]
        out.append("    { %s, %d, %d, %d, %d, %d }," % (c_str(e["name"]), e["mm"], e["zoom"], e["w50"], e["floor_pct"], e["fine"]))
    out.append('    { "", 0, 0, 0, 0, 0 }' if not keys else '    { "", 0, 0, 0, 0, 0 }   /* sentinel */')
    out.append("};")
    return "\n".join(out) + "\n"


def cmd_export(args):
    text = header_text(read_db(args.db))
    with open(args.out, "w") as f:
        f.write(text)
    print("wrote %s" % args.out)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", default=DEFAULT_DB, help="repo profile file (default data/af_lens_profiles.txt)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("import", help="merge AFLENS.TXT files / card folders into the repo file")
    p.add_argument("paths", nargs="+")
    p.add_argument("--src", help="contributor tag (default: name of the folder the card was copied to)")
    p.set_defaults(fn=cmd_import)
    p = sub.add_parser("list", help="print the repo file")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("export-header", help="generate modules/crop_rec/af_lens_db.h")
    p.add_argument("--out", default=DEFAULT_HEADER)
    p.set_defaults(fn=cmd_export)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
