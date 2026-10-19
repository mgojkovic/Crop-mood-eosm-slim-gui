#!/usr/bin/env python3
"""One command for every offline AF test tier (no camera, no card needed).

  python3 contrib/af_test/run_all.py [--verbose] [--trials N]

Tiers
  1  AFSWEEP parser self-test        contrib/af_replay/af_replay.py --selftest
  2  unit + simulator suite          contrib/af_replay/tests/ (stdlib unittest)
  3  synthetic-log CLI pipeline      synth_logs.py -> af_replay.py -> af_report.py
  4  field-log regression corpus     contrib/af_test/corpus/<case>/ (skipped while empty)

Prints a PASS/FAIL summary with counts and exits non-zero if anything fails.
`make af-test` runs exactly this.

Python 3 standard library only.
"""
import argparse
import io
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
AF_REPLAY_DIR = os.path.join(REPO, 'contrib', 'af_replay')
TESTS_DIR = os.path.join(AF_REPLAY_DIR, 'tests')
AF_TEST_TESTS_DIR = os.path.join(HERE, 'tests')
AF_REPLAY = os.path.join(AF_REPLAY_DIR, 'af_replay.py')
AF_REPORT = os.path.join(REPO, 'contrib', 'af_report', 'af_report.py')

if AF_REPLAY_DIR not in sys.path:
    sys.path.insert(0, AF_REPLAY_DIR)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import corpus          # noqa: E402
import synth_logs      # noqa: E402


def _run(script, *args, timeout=180):
    return subprocess.run([sys.executable, script] + [str(a) for a in args],
                          capture_output=True, text=True, timeout=timeout, cwd=REPO)


def tier_parser_selftest(verbose=False):
    t0 = time.time()
    p = _run(AF_REPLAY, '--selftest')
    ok = p.returncode == 0 and 'AFSWEEP parser self-test: ok' in p.stdout
    detail = 'AFSWEEP parser self-test: ok' if ok else (p.stdout + p.stderr).strip()[-500:]
    if verbose:
        print(p.stdout, end='')
    return dict(ok=ok, detail=detail, secs=time.time() - t0)


def tier_unittest(verbose=False):
    t0 = time.time()
    stream = io.StringIO()
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    # af_replay's parser/simulator/algorithm tests plus af_test's own tool tests
    # (e.g. the AFSTOP stop-halt analyzer); both discover as stdlib unittest suites
    for tests_dir in (TESTS_DIR, AF_TEST_TESTS_DIR):
        if os.path.isdir(tests_dir):
            suite.addTests(loader.discover(tests_dir))
    runner = unittest.TextTestRunner(stream=stream, verbosity=2 if verbose else 1)
    result = runner.run(suite)
    out = stream.getvalue()
    counts = dict(run=result.testsRun, failed=len(result.failures), errors=len(result.errors),
                  skipped=len(result.skipped))
    counts['passed'] = counts['run'] - counts['failed'] - counts['errors'] - counts['skipped']
    ok = not result.failures and not result.errors
    if verbose or not ok:
        print(out, end='')
    detail = ' '.join('%s=%d' % (k, counts[k]) for k in ('run', 'passed', 'failed', 'errors', 'skipped'))
    return dict(ok=ok, detail=detail, secs=time.time() - t0, counts=counts, output=out)


def tier_pipeline(verbose=False, trials=2):
    """Generate a synthetic session and run both CLIs end to end, without a card."""
    t0 = time.time()
    tmp = tempfile.mkdtemp(prefix='af_pipeline_')
    logs = os.path.join(tmp, 'logs')
    report = os.path.join(tmp, 'REPORT.md')
    try:
        meta = synth_logs.generate(logs)
        if verbose:
            print('  generated %d synthetic logs in %s' % (len(meta['files']), logs))
        p = _run(AF_REPLAY, '--logs', logs, '--trials', trials, '--report', report, '--seed', 'pipeline')
        if p.returncode != 0:
            return dict(ok=False, secs=time.time() - t0, detail=('af_replay failed: ' + (p.stdout + p.stderr))[-800:])
        if not os.path.isfile(report):
            return dict(ok=False, secs=time.time() - t0, detail='af_replay wrote no report')
        with open(report) as f:
            rep = f.read()
        missing = [x for x in ('## 3. Results', '## 4. Recommendation') if x not in rep]
        if missing:
            return dict(ok=False, secs=time.time() - t0, detail='report misses %s' % missing)
        q = _run(AF_REPORT, '--logs', logs, '--all', '--no-state')
        if q.returncode != 0 or '== Verdict' not in q.stdout:
            return dict(ok=False, secs=time.time() - t0,
                        detail=('af_report failed: ' + (q.stdout + q.stderr))[-800:])
        verdicts = corpus.verdict_lines(q.stdout)
        if not verdicts:
            return dict(ok=False, secs=time.time() - t0, detail='af_report produced no verdict')
        if verbose:
            print(q.stdout)
        return dict(ok=True, secs=time.time() - t0,
                    detail='%d runs replayed, %d verdict(s): %s'
                           % (meta['afdrive']['runs'], len(verdicts), '; '.join(verdicts)))
    except Exception as e:                                   # a broken tool is a failure, not a crash
        return dict(ok=False, secs=time.time() - t0, detail='%s: %s' % (type(e).__name__, e))
    finally:
        import shutil
        shutil.rmtree(tmp, True)


def tier_corpus(verbose=False):
    t0 = time.time()
    summary = corpus.run_all_cases()
    ok = summary['failed'] == 0
    if summary['n'] == 0:
        detail = 'no field-log cases yet (skipped; see contrib/af_test/corpus/README.md)'
    else:
        detail = '%d case(s): %d pass, %d fail, %d skip' % (
            summary['n'], summary['passed'], summary['failed'], summary['skipped'])
    if verbose or not ok:
        for r in summary['cases']:
            print('  %-4s %s%s' % (r['status'].upper(), r['name'],
                                   ('  ' + '; '.join(r['errors'])) if r['errors'] else ''))
    return dict(ok=ok, skipped=(summary['n'] == 0), detail=detail, secs=time.time() - t0,
                corpus=summary)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Run every offline AF test tier')
    ap.add_argument('--verbose', action='store_true', help='show the full unittest/CLI output')
    ap.add_argument('--trials', type=int, default=2, help='Monte Carlo trials per cell in the pipeline tier (default 2)')
    args = ap.parse_args(argv)

    print('AF offline test suite')
    print('  repo: %s' % REPO)
    print('  python: %s' % sys.version.split()[0])
    print('')
    tiers = []
    tiers.append(('parser self-test (af_replay --selftest)', tier_parser_selftest(args.verbose)))
    tiers.append(('unit + simulator + tool suite (2 test dirs)',
                  tier_unittest(args.verbose)))
    tiers.append(('synthetic-log CLI pipeline', tier_pipeline(args.verbose, args.trials)))
    tiers.append(('field-log regression corpus', tier_corpus(args.verbose)))

    print('')
    print('=' * 96)
    print('AF TEST SUMMARY')
    print('=' * 96)
    width = max(len(name) for name, _ in tiers)
    counts = dict(run=0, passed=0, failed=0, errors=0, skipped=0)
    npass = nskip = nfail = 0
    for name, res in tiers:
        status = 'PASS' if res['ok'] else 'FAIL'
        if res.get('skipped') and res['ok']:
            status = 'SKIP'
            nskip += 1
        elif res['ok']:
            npass += 1
        else:
            nfail += 1
        print('  %-4s %-*s  %6.1fs  %s' % (status, width, name, res['secs'], res['detail']))
        for k in counts:
            counts[k] += res.get('counts', {}).get(k, 0)
    print('-' * 96)
    print('  unit tests: %d run, %d passed, %d failed, %d errors, %d skipped'
          % (counts['run'], counts['passed'], counts['failed'], counts['errors'], counts['skipped']))
    print('  tiers: %d pass, %d fail, %d skip' % (npass, nfail, nskip))
    print('=' * 96)
    if nfail:
        print('RESULT: FAIL')
        return 1
    print('RESULT: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
