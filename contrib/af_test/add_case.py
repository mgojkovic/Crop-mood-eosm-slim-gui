#!/usr/bin/env python3
"""Add a field session to the AF log regression corpus (contrib/af_test/corpus/).

After a camera session, copy the interesting logs into a new corpus case and write a
*baseline* expected.json from the current af_report output:

  python3 contrib/af_test/add_case.py --logs /Volumes/EOS_DIGITAL --name 2026-10-20-18mm-refocus
  python3 contrib/af_test/add_case.py --logs ~/af_logs/card19 --name 2026-10-20-35mm --force

Then **review expected.json and tighten it before committing**: the baseline records
what the code does today, not what it should do.  In particular set rtest.ok_at_least
to the target you want to keep (e.g. 5/5 for the 18 mm refocus test), and add
must_contain/must_not_contain verdicts for the failure mode the fix addresses.

Crash/ASSERT logs are not copied on purpose: af_report --all counts every crash log on
the card as new, which would make the case fail forever.

Python 3 standard library only.
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import sys

import corpus


def find_logs(src):
    """Recursively find the corpus-relevant logs under a card/copy directory."""
    found = {}
    for name in corpus.CORPUS_LOGS:
        for path in sorted(set(glob.glob(os.path.join(src, '**', name), recursive=True))):
            # prefer a log closer to the root (card/ML/LOGS is one level down, fine)
            found.setdefault(name, path)
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description='Add an AF field session to the regression corpus')
    ap.add_argument('--logs', required=True, help='session directory (card mount or a copied folder)')
    ap.add_argument('--name', required=True, help='case id, e.g. 2026-10-20-18mm-refocus')
    ap.add_argument('--root', default=corpus.CORPUS_DIR, help='corpus root (default contrib/af_test/corpus)')
    ap.add_argument('--force', action='store_true', help='replace an existing case')
    ap.add_argument('--note', default='', help='short note stored in expected.json')
    ap.add_argument('--build', default='', help='firmware/build description stored in the provenance')
    args = ap.parse_args(argv)

    if not os.path.isdir(args.logs):
        sys.exit('no such directory: %s' % args.logs)
    if not os.path.isdir(args.root):
        os.makedirs(args.root)
    case = os.path.join(args.root, args.name)
    if os.path.isdir(case) and not args.force:
        sys.exit('%s already exists (use --force to replace)' % case)
    logs = find_logs(args.logs)
    if not logs:
        sys.exit('no known AF logs found under %s (looked for %s)'
                 % (args.logs, ', '.join(corpus.CORPUS_LOGS)))
    if os.path.isdir(case):
        shutil.rmtree(case)
    os.makedirs(case)
    for name, path in sorted(logs.items()):
        shutil.copy2(path, os.path.join(case, name))
    print('copied %d log(s) from %s to %s: %s'
          % (len(logs), args.logs, case, ', '.join(sorted(logs))))

    # baseline expected.json from what the current tools say about this session
    text = corpus.report_text(case)
    metrics = corpus.parse_metrics(text)
    verdicts = corpus.verdict_lines(text)
    try:
        stats = corpus.replay_stats(case)
        facts = corpus.log_facts(case, stats)
    except Exception as e:
        print('warning: af_replay could not parse this session (%s: %s); facts/replay checks omitted'
              % (type(e).__name__, e))
        stats, facts = None, {}
    exp = {
        'name': args.name,
        'historical_fixture': True,
        'source': os.path.abspath(args.logs),
        'added': datetime.date.today().isoformat(),
        'notes': (args.note + ' ' if args.note else '') +
                 'BASELINE snapshot of the current build; review/tighten before committing.',
        'provenance': {
            'source_dir': os.path.abspath(args.logs),
            'copied': datetime.date.today().isoformat(),
            'build': args.build or '(fill in: firmware zip / commit)',
            'files': corpus.file_fingerprints(case),
        },
    }
    if verdicts:
        exp['verdicts'] = {'must_contain': verdicts, 'must_not_contain': []}
    if 'afdrive' in metrics:
        exp['afdrive'] = {
            'min_runs': metrics['afdrive']['runs'],
            'min_success_pct': metrics['afdrive']['success_pct'],
            'max_median_ms': metrics['afdrive'].get('median_total_ms', 5000),
        }
    if 'rtest' in metrics:
        exp['rtest'] = {'ok_at_least': metrics['rtest']['ok'],
                        'of': metrics['rtest']['of'],
                        'false_trig_max': metrics['rtest']['false_trig']}
    if 'bench' in metrics and 'B' in metrics['bench']:
        exp['bench'] = {'algo': 'B', 'min_ok': metrics['bench']['B']['ok'], 'of': metrics['bench']['B']['n']}
    if facts:
        # drop the per-sweep dicts from the baseline file: the aggregate scope facts are what a
        # reviewer tightens, the full series list lives in the log itself
        facts = json.loads(json.dumps(facts))
        if 'sweep' in facts:
            facts['sweep'].pop('series', None)
        exp['facts'] = facts
    if stats and (stats['runs'] or stats['sweeps']):
        exp['replay'] = dict(min_runs=stats['runs'], min_moves=stats['moves'],
                             min_series=stats['series'], min_curve_fits=stats['curve_fits'])
        if stats['sweeps']:
            exp['replay'].update(min_sweeps=stats['sweeps'],
                                 min_sweep_samples=sum(x['samples'] for x in stats['sweep_fits']))
    if 'rtest' in metrics:
        # target, not baseline: the verdict the fix is supposed to deliver
        exp['post_patch_expectation'] = {
            'status': 'TARGET, not current: review and tighten after the fix is flashed',
            'rtest': {'ok_at_least': min(metrics['rtest']['of'], max(4, metrics['rtest']['ok'])),
                      'of': metrics['rtest']['of'],
                      'all_trials_triggered': True},
            'note': 'Baseline is what the current build does. Update this target only with a '
                    'post-patch field session, kept as a separate corpus case.',
        }
    corpus_expected = os.path.join(case, 'expected.json')
    with open(corpus_expected, 'w') as f:
        json.dump(exp, f, indent=2)
        f.write('\n')

    print('wrote %s' % corpus_expected)
    print('')
    print('Baseline checks:')
    for key in ('verdicts', 'afdrive', 'rtest', 'bench', 'facts', 'replay', 'post_patch_expectation'):
        if key in exp:
            print('  %-20s %s' % (key, json.dumps(exp[key])[:160]))
    print('')
    print('Next: review expected.json, tighten the limits to the target behaviour, then run')
    print('  python3 contrib/af_test/run_all.py')
    print('  python3 contrib/af_test/corpus.py -v')
    return 0


if __name__ == '__main__':
    sys.exit(main())
