#!/usr/bin/env python3
"""AFSTOP halt analysis: does a `(9,3,1)` stop actually stop a `(9,1)` relative move?

The AF stop test (camera: AF diag -> AF stop test, mode 5) writes `AFSTOP.LOG`: one
section per move with a `# stop_plan k=.. at=<ms>` line, a stream of sharpness samples,
and a `# stop k=.. t_stop=.. t_done=..` line.  The question the test exists to answer is
whether the stop command halts the lens, because algorithms C and G and any predictive
braking depend on it.

The analysis compares the sharpness *at* the stop request with the samples after it:

  s_at_stop          last s1 sample at or before t_stop (what the lens saw when stopped)
  s_end              last s1 sample of the trial
  delta_pct          100 * (s_end - s_at_stop) / s_at_stop
  post_stop_slope    least-squares slope of s1 over samples t >= t_stop, in %/s of
                     s_at_stop; the robust "is it still moving" measure
  max_excursion_pct  largest signed s1 change after t_stop in the move direction
                     (positive = kept moving the commanded way, negative = reversed)
  halted             True when |slope| <= HALT_SLOPE_PCT_S and |excursion| <= HALT_EXCURSION_PCT
  continued          not halted, and the excursion was in the commanded direction

Sample formats accepted (see af_replay.py):
  S <t_ms> <frame> <s1> <s2> <s4> <g> <q> <band> <luma>    documented full format (10 tokens)
  S <t_ms> <s1> <g> <luma>                                 shipped crop_rec.c format (4 values)

`card9`'s AFSTOP.LOG has the full 10-column shape but s2/s4/band are all zero; the
analysis uses s1 for every trial and reports those zero columns rather than failing.

CLI
  python3 contrib/af_test/stop_analysis.py [PATH ...]   # PATH = AFSTOP.LOG or a directory
  python3 contrib/af_test/stop_analysis.py --json       # machine-readable rows + summary

With no PATH it scans contrib/af_test/corpus for AFSTOP.LOG files, i.e. the real
2026-10-19 card8/card9 stop cases.

Exit codes
  0  every parsed trial classified "not halted" (the historical, expected evidence:
     stop does NOT halt the move)
  1  at least one trial classified "halted": the stop appears to work, i.e. the evidence
     changed and the C/G/predictive-braking assumptions must be re-examined
  2  no complete stop trial could be parsed (bad path, truncated log, unknown format)

Python 3 standard library only; also importable (`parse_file`, `analyze_file`,
`classify_trial`, `summarize`, `analyze_paths`).
"""
import argparse
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_DIR = os.path.join(HERE, 'corpus')

# Classifier thresholds.  The real 2026-10-19 traces have a least-squares post-stop
# slope of 8.3-406 %/s (minimum |slope| 8.3 %/s), so 2 %/s (the lead analysis' "none
# < 2 %/s" bound) separates a frozen trace from every observed one with a wide margin.
HALT_SLOPE_PCT_S = 2.0
HALT_EXCURSION_PCT = 1.0

AFSTOP_HEAD_RE = re.compile(r'^#\s*AFSTOP\b(.*)$')
STOP_PLAN_RE = re.compile(r'^#\s*stop_plan\s+k=(\d+)\b(.*)$')
STOP_RE = re.compile(r'^#\s*stop\s+k=(\d+)\b(.*)$')
DONE_RE = re.compile(r'^#\s*done\s+k=(\d+)\b(.*)$')
SWEEP_RE = re.compile(r'^#\s*sweep\s+k=(\d+)\b(.*)$')
LAND_RE = re.compile(r'^#\s*land\s+k=(\d+)\b(.*)$')
SAMPLE_RE = re.compile(r'^S\s')

KV_RE = re.compile(r'(\w+)=("[^"]*"|\S+)')


# --------------------------------------------------------------------------- parsing

def _kv(line):
    """`k="quoted string" k2=123` -> dict of raw strings (quotes stripped)."""
    out = {}
    for key, val in KV_RE.findall(line):
        if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
            val = val[1:-1]
        out[key] = val
    return out


def _int(kv, key, default=None):
    try:
        return int(kv[key], 0)
    except (KeyError, ValueError):
        return default


def _sample(line):
    """One `S ...` line -> dict, or None for an unknown field count."""
    parts = line.split()
    if len(parts) >= 10:
        # documented full format: S t_ms frame s1 s2 s4 g q band luma
        try:
            return dict(t=int(parts[1]), frame=int(parts[2]), s1=float(parts[3]),
                        s2=float(parts[4]), s4=float(parts[5]), g=float(parts[6]),
                        q=float(parts[7]), band=int(parts[8]), luma=float(parts[9]),
                        format='full')
        except ValueError:
            return None
    if len(parts) == 5:
        # shipped crop_rec.c format: S t_ms s1 g luma (the "4-field" line)
        try:
            s1 = float(parts[2])
            return dict(t=int(parts[1]), frame=0, s1=s1, s2=s1, s4=s1,
                        g=float(parts[3]), q=0.0, band=0, luma=float(parts[4]),
                        format='shipped')
        except ValueError:
            return None
    return None


def _card_name(path):
    """Best card id for a log path: a `card<N>...` path component, else parent dir."""
    for part in reversed(os.path.abspath(path).split(os.sep)):
        m = re.match(r'^(card\d+)', part)
        if m:
            return m.group(1)
    return os.path.basename(os.path.dirname(os.path.abspath(path))) or '?'


def parse_text(text, source=None):
    """Parse AFSTOP log text -> list of sessions.

    A session is one `# AFSTOP` block (a stop test run with several k trials):
        dict(index, header, trials=[trial, ...])
    A trial:
        dict(k, at, dir, speed, cmd, start_rel, t_stop, t_done, stop_val, sent,
             land, samples=[sample, ...], sample_format)
    A sample is what `_sample()` returns (t, frame, s1, s2, s4, g, q, band, luma).
    Missing fields are None; the classifier decides whether the trial is complete.
    """
    sessions = []
    session = None
    trial = None
    pending = {}                       # k -> {'sweep': {...}, 'land': {...}}
    path = source or '<text>'

    def close_trial():
        nonlocal trial
        if trial is None:
            return
        trial['sample_format'] = (trial['samples'][0]['format']
                                  if trial['samples'] else None)
        trial['samples_ignored'] = trial.pop('_ignored', 0)
        session['trials'].append(trial)
        trial = None

    def close_session():
        nonlocal session
        close_trial()
        if session is not None:
            sessions.append(session)
            session = None
        pending.clear()

    for line in text.splitlines():
        line = line.rstrip('\r\n')
        if not line:
            continue
        m = AFSTOP_HEAD_RE.match(line)
        if m:
            close_session()
            session = dict(index=len(sessions) + 1, header=_kv(m.group(1)), trials=[])
            continue
        if session is None:
            # tolerate logs that start with a stop_plan but lost the header line
            session = dict(index=1, header={}, trials=[])
        m = SWEEP_RE.match(line)
        if m:
            pending.setdefault(int(m.group(1)), {})['sweep'] = _kv(m.group(2))
            continue
        m = LAND_RE.match(line)
        if m:
            pending.setdefault(int(m.group(1)), {})['land'] = _kv(m.group(2))
            continue
        m = STOP_PLAN_RE.match(line)
        if m:
            close_trial()
            kv = _kv(m.group(2))
            k = int(m.group(1))
            pre = pending.pop(k, {})
            sweep = pre.get('sweep') or {}
            trial = dict(
                file=os.path.basename(path), path=os.path.abspath(path),
                k=k, at=_int(kv, 'at'), dir_=_int(kv, 'dir', _int(sweep, 'dir')),
                speed=kv.get('speed') or sweep.get('speed'),
                cmd=_int(kv, 'cmd', _int(sweep, 'cmd')),
                start_rel=_int(kv, 'start_rel'),
                land=pre.get('land'), samples=[], t_stop=None, t_done=None,
                stop_val=None, sent=None)
            continue
        m = STOP_RE.match(line)
        if m and trial is not None and trial['k'] == int(m.group(1)):
            kv = _kv(m.group(2))
            trial['t_stop'] = _int(kv, 't_stop')
            trial['t_done'] = _int(kv, 't_done')
            trial['stop_val'] = kv.get('val')
            trial['sent'] = _int(kv, 'sent')
            continue
        m = DONE_RE.match(line)
        if m and trial is not None and trial['k'] == int(m.group(1)):
            kv = _kv(m.group(2))
            trial['done_t'] = _int(kv, 't')
            continue
        if SAMPLE_RE.match(line) and trial is not None:
            smp = _sample(line)
            if smp is None:
                trial['_ignored'] = trial.get('_ignored', 0) + 1
            else:
                trial['samples'].append(smp)
            continue
    close_session()
    # `dir` is not a Python builtin but keeps the row/JSON naming obvious
    for sess in sessions:
        for tr in sess['trials']:
            tr['dir'] = tr.pop('dir_')
    return sessions


def parse_file(path):
    """Parse one AFSTOP.LOG -> dict(path, file, card, sessions)."""
    with open(path, errors='replace') as f:
        text = f.read()
    return dict(path=os.path.abspath(path), file=os.path.basename(path),
                card=_card_name(path), sessions=parse_text(text, source=path))


# --------------------------------------------------------------------------- classifier

def _ols_slope(points):
    """Least-squares slope of ([x], [y]); 0 for fewer than two distinct x values."""
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    if len(set(xs)) < 2:
        return 0.0
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    den = sum((x - mx) ** 2 for x in xs)
    if not den:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den


def classify_trial(trial, halt_slope=HALT_SLOPE_PCT_S, halt_excursion=HALT_EXCURSION_PCT):
    """Compute the post-stop facts for one parsed trial -> row dict.

    row keys: k, at, dir, t_stop, t_done, sample_format, n_post,
              s_at_stop, s_end, delta_pct, post_stop_ms,
              post_stop_slope_pct_s, max_excursion_pct, halted, continued,
              classification ('halted' | 'continued' | 'changed' | 'incomplete'),
              reason (only when incomplete)
    """
    row = dict(k=trial['k'], at=trial['at'], dir=trial['dir'],
               t_stop=trial['t_stop'], t_done=trial['t_done'],
               sample_format=trial.get('sample_format'),
               land=trial.get('land'), n_post=0,
               s_at_stop=None, s_end=None, delta_pct=None, post_stop_ms=None,
               post_stop_slope_pct_s=None, max_excursion_pct=None,
               halted=None, continued=None, classification='incomplete')
    samples = trial.get('samples') or []
    if trial.get('t_stop') is None:
        row['reason'] = 'no "# stop k=.. t_stop=.." line'
        return row
    if len(samples) < 2:
        row['reason'] = 'fewer than 2 sharpness samples'
        return row

    before = [s for s in samples if s['t'] <= trial['t_stop']]
    after = [s for s in samples if s['t'] >= trial['t_stop']]
    row['n_post'] = len(after)
    base = before[-1] if before else after[0]
    s0 = base['s1']
    row['s_at_stop'] = s0
    row['s_end'] = after[-1]['s1'] if after else samples[-1]['s1']
    if not s0:
        row['reason'] = 's1 is zero at the stop sample'
        return row
    row['delta_pct'] = round(100.0 * (row['s_end'] - s0) / s0, 2)
    if len(after) < 2:
        row['reason'] = 'fewer than 2 post-stop samples'
        return row
    row['post_stop_ms'] = after[-1]['t'] - trial['t_stop']
    if row['post_stop_ms'] <= 0:
        row['reason'] = 'non-increasing post-stop timestamps'
        return row
    slope = _ols_slope([(s['t'] / 1000.0, s['s1']) for s in after])
    row['post_stop_slope_pct_s'] = round(100.0 * slope / s0, 1)
    direction = trial['dir']
    if direction:
        # signed largest change after the stop: positive = still going the commanded way
        if direction > 0:
            extreme = max(s['s1'] for s in after)
        else:
            extreme = min(s['s1'] for s in after)
        row['max_excursion_pct'] = round(100.0 * (extreme - s0) / s0, 2)
        excursion = row['max_excursion_pct']
    else:
        excursion = max(abs(100.0 * (s['s1'] - s0) / s0) for s in after)
        row['max_excursion_pct'] = round(excursion, 2)
    row['halted'] = bool(abs(row['post_stop_slope_pct_s']) <= halt_slope
                         and abs(excursion) <= halt_excursion)
    row['continued'] = bool(not row['halted'] and direction
                            and excursion * direction >= halt_excursion)
    if row['halted']:
        row['classification'] = 'halted'
    elif row['continued']:
        row['classification'] = 'continued'
    else:
        row['classification'] = 'changed'
    return row


def _zero_columns(samples):
    """Full-format columns that are zero in every sample (e.g. card9's s2/s4/band)."""
    if not samples or samples[0]['format'] != 'full':
        return []
    return [col for col in ('s2', 's4', 'g', 'q', 'band', 'luma')
            if all(not s.get(col) for s in samples)]


def summarize(rows):
    """Aggregate the per-trial rows -> summary dict with the halt verdict and exit code."""
    complete = [r for r in rows if r['classification'] != 'incomplete']
    halted = [r for r in complete if r['halted']]
    continued = [r for r in complete if r['continued']]
    changed = [r for r in complete if r['classification'] == 'changed']
    slopes = [abs(r['post_stop_slope_pct_s']) for r in complete
              if r['post_stop_slope_pct_s'] is not None]
    deltas = [abs(r['delta_pct']) for r in complete if r['delta_pct'] is not None]
    excursions = [abs(r['max_excursion_pct']) for r in complete
                  if r['max_excursion_pct'] is not None]
    planned = sorted(set(r['at'] for r in complete if r['at'] is not None))
    if not complete:
        verdict = 'INCONCLUSIVE: no complete stop trial'
        code = 2
    elif halted:
        verdict = ('STOP APPEARS TO HALT: %d/%d trialse(s) froze after t_stop - '
                   'evidence changed, re-examine C/G/predictive-braking assumptions'
                   % (len(halted), len(complete)))
        code = 1
    else:
        verdict = ('STOP DOES NOT HALT THE MOVE: %d/%d trial(s) kept changing after '
                   't_stop (%d continued in the move direction)'
                   % (len(complete), len(complete), len(continued)))
        code = 0
    return dict(
        trials=len(rows), complete=len(complete), incomplete=len(rows) - len(complete),
        halted=len(halted), continued=len(continued), changed=len(changed),
        planned_stop_ms=planned,
        median_abs_slope_pct_s=round(statistics.median(slopes), 1) if slopes else None,
        median_abs_delta_pct=round(statistics.median(deltas), 2) if deltas else None,
        median_abs_excursion_pct=round(statistics.median(excursions), 2) if excursions else None,
        min_abs_slope_pct_s=round(min(slopes), 1) if slopes else None,
        verdict=verdict, exit_code=code)


def analyze_file(path, halt_slope=HALT_SLOPE_PCT_S, halt_excursion=HALT_EXCURSION_PCT):
    """Parse + classify one AFSTOP.LOG -> dict(path, file, card, sessions, rows, summary)."""
    parsed = parse_file(path)
    rows = []
    zero_columns = set()
    for sess in parsed['sessions']:
        all_samples = [s for tr in sess['trials'] for s in tr['samples']]
        zero_columns.update(_zero_columns(all_samples))
        for tr in sess['trials']:
            row = classify_trial(tr, halt_slope, halt_excursion)
            row['card'] = parsed['card']
            row['file'] = parsed['file']
            row['session'] = sess['index']
            rows.append(row)
        sess['rows'] = [r for r in rows if r['session'] == sess['index']]
    summary = summarize(rows)
    summary['zero_columns'] = sorted(zero_columns)
    return dict(path=parsed['path'], file=parsed['file'], card=parsed['card'],
                sessions=len(parsed['sessions']), rows=rows, summary=summary)


def find_afstop_logs(paths):
    """Expand files/directories to a sorted list of AFSTOP.LOG paths."""
    found = []
    for path in paths:
        if os.path.isdir(path):
            for root, dirs, files in os.walk(path):
                dirs[:] = [d for d in dirs if not d.startswith('.')]
                for name in files:
                    if name.upper() == 'AFSTOP.LOG':
                        found.append(os.path.join(root, name))
        elif os.path.isfile(path):
            found.append(path)
    return sorted(set(os.path.abspath(p) for p in found))


def analyze_paths(paths=None, halt_slope=HALT_SLOPE_PCT_S, halt_excursion=HALT_EXCURSION_PCT):
    """Analyze every AFSTOP.LOG under paths -> aggregate dict (rows + summary)."""
    paths = paths if paths is not None else [CORPUS_DIR]
    files = find_afstop_logs(paths)
    results = [analyze_file(p, halt_slope, halt_excursion) for p in files]
    rows = [r for res in results for r in res['rows']]
    summary = summarize(rows)
    summary['files'] = len(results)
    summary['sessions'] = sum(res['sessions'] for res in results)
    zero = sorted(set(c for res in results for c in res['summary']['zero_columns']))
    summary['zero_columns'] = zero
    summary['paths'] = [res['path'] for res in results]
    return dict(results=results, rows=rows, summary=summary)


# --------------------------------------------------------------------------- output

TABLE_HEADER = ('  #  card    sess  k  dir   at  t_stop  t_done   s@stop     s_end  '
                'delta%  slope%/s   exc%  halted  class')
TABLE_FMT = ('{i:>3}  {card:<6} {sess:>4} {k:>2} {dir:>4} {at:>4} {t_stop:>7} '
             '{t_done:>7} {s0:>8} {s1:>8} {delta:>7} {slope:>9} {exc:>7}  '
             '{halted:<6}  {cls}')


def _fmt(value, spec, fallback='-'):
    if value is None:
        return fallback.rjust(len(spec % 0)) if spec else fallback
    return spec % value


def format_table(rows):
    """The per-trial table as text (the primary human output of the tool)."""
    out = [TABLE_HEADER]
    for i, r in enumerate(rows, 1):
        exc = r['max_excursion_pct']
        out.append(TABLE_FMT.format(
            i=i, card=(r.get('card') or '?')[:6], sess=r.get('session') or 0, k=r['k'],
            dir=('%+d' % r['dir']) if r['dir'] else '?',
            at=_fmt(r['at'], '%d'), t_stop=_fmt(r['t_stop'], '%d'),
            t_done=_fmt(r['t_done'], '%d'),
            s0=_fmt(r['s_at_stop'], '%.0f'), s1=_fmt(r['s_end'], '%.0f'),
            delta=_fmt(r['delta_pct'], '%+.2f'),
            slope=_fmt(r['post_stop_slope_pct_s'], '%+.1f'),
            exc=_fmt(exc, '%+.2f'),
            halted=('n/a' if r['halted'] is None else ('yes' if r['halted'] else 'no')),
            cls=r['classification']))
    return '\n'.join(out)


def format_summary(summary):
    """Human-readable summary + the verdict line and its meaning."""
    s = summary
    out = ['stop-halt analysis: %d file(s), %d session(s), %d trial(s)'
           % (s.get('files', 0), s.get('sessions', 0), s['trials']),
           '  planned stop times: %s ms'
           % (', '.join(str(x) for x in s['planned_stop_ms']) or '-'),
           '  classified: %d halted, %d not halted (%d continued in the move direction, '
           '%d changed, %d incomplete)'
           % (s['halted'], s['complete'] - s['halted'], s['continued'], s['changed'],
              s['incomplete'])]
    if s['median_abs_slope_pct_s'] is not None:
        out.append('  median post-stop |slope|: %.1f %%/s (min %.1f); median |delta|: %.2f %%; '
                   'median |max excursion|: %.2f %%'
                   % (s['median_abs_slope_pct_s'], s['min_abs_slope_pct_s'],
                      s['median_abs_delta_pct'], s['median_abs_excursion_pct']))
    if s.get('zero_columns'):
        out.append('  note: %s column(s) are zero in every full-format sample '
                   '(trimmed build); s1 is used for the classification.'
                   % ', '.join(s['zero_columns']))
    out.append('  halted=0 means the sharpness kept changing after t_stop: the stop did '
               'NOT freeze the lens.')
    out.append('  verdict: ' + s['verdict'])
    out.append('  exit %d' % s['exit_code'])
    return '\n'.join(out)


def _json_result(result):
    return dict(
        files=result['summary'].get('files', 0),
        sessions=result['summary'].get('sessions', 0),
        rows=result['rows'],
        summary=dict((k, v) for k, v in result['summary'].items() if k != 'paths'))


# --------------------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Does an AFSTOP stop halt the AF move? (offline analysis of AFSTOP.LOG)')
    ap.add_argument('paths', nargs='*',
                    help='AFSTOP.LOG file(s) or directories (default: contrib/af_test/corpus)')
    ap.add_argument('--json', action='store_true', help='print rows + summary as JSON')
    ap.add_argument('--no-table', action='store_true', help='print only the summary')
    ap.add_argument('--halt-slope', type=float, default=HALT_SLOPE_PCT_S,
                    help='halted if the post-stop least-squares slope is <= this (%%%%/s, '
                         'default %.1f)' % HALT_SLOPE_PCT_S)
    ap.add_argument('--halt-excursion', type=float, default=HALT_EXCURSION_PCT,
                    help='halted if the post-stop max excursion is <= this (%%%%/s, default %.1f)'
                         % HALT_EXCURSION_PCT)
    args = ap.parse_args(argv)
    result = analyze_paths(args.paths or [CORPUS_DIR],
                           halt_slope=args.halt_slope,
                           halt_excursion=args.halt_excursion)
    if args.json:
        print(json.dumps(_json_result(result), indent=2, sort_keys=True))
    else:
        if not args.no_table and result['rows']:
            print(format_table(result['rows']))
            print('')
        print(format_summary(result['summary']))
    return result['summary']['exit_code']


if __name__ == '__main__':
    sys.exit(main())
