#!/usr/bin/env python3
"""Unit tests for contrib/af_test/stop_analysis.py and the AFSTOP corpus cases.

Runs with the rest of the offline suite (tier 2):

  python3 -m unittest discover -s contrib/af_test/tests -v
  python3 contrib/af_test/run_all.py

The real 2026-10-19 card8/card9 AFSTOP logs are the ground truth: all 12 stop trials
must classify as "continued" (stop does NOT halt the move).  A synthetic trace whose
sharpness is constant after t_stop must classify as "halted" instead, so a future build
that fixes stop cannot silently match the old evidence.
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AF_TEST_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(AF_TEST_DIR))
CORPUS_REAL = os.path.join(AF_TEST_DIR, 'corpus', 'real-20261019')

if AF_TEST_DIR not in sys.path:
    sys.path.insert(0, AF_TEST_DIR)

import corpus          # noqa: E402
import stop_analysis   # noqa: E402

CARD8 = os.path.join(CORPUS_REAL, 'card8-stop')
CARD9 = os.path.join(CORPUS_REAL, 'card9-stop')
CARD8_LOG = os.path.join(CARD8, 'AFSTOP.LOG')
CARD9_LOG = os.path.join(CARD9, 'AFSTOP.LOG')

HEADER = ('# AFSTOP t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 '
          'crop_preset=2 frame_ms=42 meas_ms=31\n'
          '# meas_us=30000 s4_step=4 done_timeout=3000 extra_frames=2\n')


def _full_sample(t, s1, s2=None, s4=None, g=20000, q=300, band=146, luma=100):
    """One documented 10-token `S` line."""
    return 'S %d %d %d %d %d %d %d %d %d\n' % (
        t, t // 40, s1, s1 if s2 is None else s2, s1 if s4 is None else s4,
        g, q, band, luma)


def _shipped_sample(t, s1, g=20000, luma=100):
    """One shipped 4-value `S t_ms s1 g luma` line."""
    return 'S %d %d %d %d\n' % (t, s1, g, luma)


def _log(samples, plan='# stop_plan k=1 at=100\n', stop='# stop k=1 t_stop=100 t_done=500 '
         'val=4e0400 sent=400\n', done='# done k=1 t=500 val=4e0400 n=8\n',
         sweep='# sweep k=1 cmd=400 speed=7 dir=1\n'):
    return HEADER + sweep + plan + samples + stop + done


class StopTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp(prefix='af_stop_')

    def tearDown(self):
        shutil.rmtree(self._tmp, True)

    def write_log(self, text, name='AFSTOP.LOG', case='case1'):
        d = os.path.join(self._tmp, case)
        if not os.path.isdir(d):
            os.makedirs(d)
        path = os.path.join(d, name)
        with open(path, 'w') as f:
            f.write(text)
        return path

    def run_cli(self, path):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = stop_analysis.main([path])
        return code, out.getvalue()


class ParseRealLogs(StopTestCase):
    def test_card8_two_sessions_eight_trials(self):
        parsed = stop_analysis.parse_file(CARD8_LOG)
        self.assertEqual(parsed['card'], 'card8')
        self.assertEqual(len(parsed['sessions']), 2)
        trials = [t for s in parsed['sessions'] for t in s['trials']]
        self.assertEqual(len(trials), 8)
        self.assertEqual([t['k'] for t in trials], [1, 2, 3, 4, 1, 2, 3, 4])
        self.assertEqual([t['at'] for t in trials], [150, 250, 350, 450] * 2)
        self.assertEqual([t['dir'] for t in trials], [1, -1, 1, -1] * 2)
        self.assertTrue(all(t['sample_format'] == 'full' for t in trials))
        # card8 logs the direction on the `# sweep` line, not on `# stop_plan`
        self.assertTrue(all(t['cmd'] in (400, -400) for t in trials))
        self.assertEqual(trials[0]['t_stop'], 154)
        self.assertEqual(trials[0]['t_done'], 510)
        self.assertEqual(trials[0]['sent'], None)

    def test_card9_single_session_trimmed_columns(self):
        parsed = stop_analysis.parse_file(CARD9_LOG)
        self.assertEqual(parsed['card'], 'card9')
        self.assertEqual(len(parsed['sessions']), 1)
        trials = parsed['sessions'][0]['trials']
        self.assertEqual(len(trials), 4)
        self.assertEqual([t['at'] for t in trials], [80, 80, 160, 160])
        self.assertEqual([t['dir'] for t in trials], [1, -1, 1, -1])
        # direction comes from `# stop_plan`, not from a `# sweep` line
        self.assertTrue(all(t['dir'] is not None for t in trials))
        self.assertEqual(trials[0]['sent'], 100)
        self.assertTrue(all(s['s2'] == 0 and s['s4'] == 0 for t in trials
                            for s in t['samples']))
        res = stop_analysis.analyze_file(CARD9_LOG)
        self.assertEqual(res['summary']['zero_columns'], ['band', 's2', 's4'])

    def test_find_afstop_logs_recursive(self):
        found = stop_analysis.find_afstop_logs([os.path.join(AF_TEST_DIR, 'corpus')])
        self.assertIn(os.path.abspath(CARD8_LOG), found)
        self.assertIn(os.path.abspath(CARD9_LOG), found)


class ClassifyRealLogs(StopTestCase):
    def test_all_12_real_trials_continue(self):
        result = stop_analysis.analyze_paths([CARD8_LOG, CARD9_LOG])
        rows = result['rows']
        summary = result['summary']
        self.assertEqual(len(rows), 12)
        self.assertEqual(summary['files'], 2)
        self.assertEqual(summary['sessions'], 3)
        self.assertEqual(summary['halted'], 0)
        self.assertEqual(summary['continued'], 12)
        self.assertEqual(summary['changed'], 0)
        self.assertEqual(summary['incomplete'], 0)
        self.assertEqual(summary['planned_stop_ms'], [80, 150, 160, 250, 350, 450])
        # the real evidence has a large post-stop change, far from the halt threshold
        self.assertGreater(summary['min_abs_slope_pct_s'],
                           stop_analysis.HALT_SLOPE_PCT_S)
        self.assertGreater(summary['median_abs_slope_pct_s'], 20.0)
        for row in rows:
            self.assertIs(row['halted'], False, row)
            self.assertTrue(row['continued'], row)
            self.assertNotEqual(row['classification'], 'halted')
        # card8's 4th trial of the first run is the weakest: net delta ~0 but the
        # least-squares slope is ~29 %/s, which must still classify as not halted
        weak = [r for r in rows if r['card'] == 'card8' and r['session'] == 1
                and r['k'] == 4][0]
        self.assertLess(abs(weak['delta_pct']), 1.0)
        self.assertGreater(abs(weak['post_stop_slope_pct_s']),
                           stop_analysis.HALT_SLOPE_PCT_S)
        self.assertFalse(weak['halted'])

    def test_cli_exit_zero_when_stop_does_not_halt(self):
        code, out = self.run_cli(os.path.join(AF_TEST_DIR, 'corpus'))
        self.assertEqual(code, 0, out)
        self.assertIn('STOP DOES NOT HALT THE MOVE', out)
        self.assertIn('0 halted, 12 not halted', out)


class SyntheticHaltedTrace(StopTestCase):
    def test_constant_sharpness_classifies_halted(self):
        samples = ''.join(_full_sample(t, 10000) for t in (0, 40, 80, 120, 160, 200, 240))
        path = self.write_log(_log(samples))
        res = stop_analysis.analyze_file(path)
        row = res['rows'][0]
        self.assertTrue(row['halted'], row)
        self.assertFalse(row['continued'])
        self.assertEqual(row['classification'], 'halted')
        self.assertAlmostEqual(row['post_stop_slope_pct_s'], 0.0)
        self.assertEqual(res['summary']['halted'], 1)
        self.assertEqual(res['summary']['exit_code'], 1)
        code, out = self.run_cli(path)
        self.assertEqual(code, 1, out)
        self.assertIn('STOP APPEARS TO HALT', out)

    def test_small_noise_still_classifies_halted(self):
        # a real halted lens would still show measurement jitter; +-0.5 % is inside
        # the classifier's 2 %/s slope / 1 % excursion thresholds
        vals = (10000, 10040, 9990, 10020, 9995, 10010, 10000)
        samples = ''.join(_full_sample(t, v) for t, v in
                          zip((0, 40, 80, 120, 160, 200, 240), vals))
        res = stop_analysis.analyze_file(self.write_log(_log(samples)))
        self.assertTrue(res['rows'][0]['halted'], res['rows'][0])
        self.assertEqual(res['summary']['exit_code'], 1)

    def test_continued_trace_is_not_halted(self):
        vals = (10000, 10400, 10800, 11200, 11600, 12000, 12400)
        samples = ''.join(_full_sample(t, v) for t, v in
                          zip((0, 40, 80, 120, 160, 200, 240), vals))
        res = stop_analysis.analyze_file(self.write_log(_log(samples)))
        row = res['rows'][0]
        self.assertFalse(row['halted'])
        self.assertTrue(row['continued'])
        self.assertEqual(res['summary']['exit_code'], 0)

    def test_shipped_four_field_samples_parse(self):
        # shipped crop_rec.c format: S t_ms s1 g luma (4 values, no frame/s2/s4)
        vals = (10000, 10300, 10600, 10900, 11200, 11500)
        samples = ''.join(_shipped_sample(t, v) for t, v in
                          zip((0, 40, 80, 120, 160, 200), vals))
        path = self.write_log(_log(samples))
        parsed = stop_analysis.parse_file(path)
        trial = parsed['sessions'][0]['trials'][0]
        self.assertEqual(trial['sample_format'], 'shipped')
        self.assertEqual([s['s1'] for s in trial['samples']], list(vals))
        self.assertEqual(trial['samples'][0]['s2'], vals[0])   # mirrored
        res = stop_analysis.analyze_file(path)
        self.assertFalse(res['rows'][0]['halted'])
        self.assertTrue(res['rows'][0]['continued'])


class IncompleteAndAmbiguous(StopTestCase):
    def test_no_stop_trials_exits_2(self):
        path = self.write_log(HEADER)
        code, out = self.run_cli(path)
        self.assertEqual(code, 2, out)
        self.assertIn('no complete stop trial', out)

    def test_missing_stop_line_is_incomplete(self):
        samples = ''.join(_full_sample(t, 10000 + 10 * t) for t in (0, 50, 100, 150))
        text = (HEADER + '# sweep k=1 cmd=400 speed=7 dir=1\n'
                '# stop_plan k=1 at=100\n' + samples)
        res = stop_analysis.analyze_file(self.write_log(text))
        self.assertEqual(res['summary']['incomplete'], 1)
        self.assertEqual(res['summary']['exit_code'], 2)


class CorpusStopCases(unittest.TestCase):
    def test_stop_cases_are_discovered(self):
        names = [os.path.basename(c) for c in corpus.case_dirs(CORPUS_REAL)]
        self.assertIn('card8-stop', names)
        self.assertIn('card9-stop', names)

    def test_stop_corpus_cases_pass(self):
        for case in (CARD8, CARD9):
            result = corpus.run_case(case)
            self.assertEqual(result['status'], 'pass',
                             '%s: %s' % (os.path.basename(case), result['errors']))
            self.assertIn('stop', result['facts'])
            self.assertEqual(result['facts']['stop']['halted_trials'], 0)
            self.assertEqual(result['facts']['stop']['trials'],
                             len(result['facts']['stop']['rows']))

    def test_tampered_halted_expectation_fails(self):
        with tempfile.TemporaryDirectory(prefix='af_stop_tamper_') as tmp:
            case = os.path.join(tmp, 'card9-stop')
            os.makedirs(case)
            shutil.copy2(CARD9_LOG, os.path.join(case, 'AFSTOP.LOG'))
            with open(os.path.join(CARD9, 'expected.json')) as f:
                exp = json.load(f)
            # pretend the fixture's first trial froze after t_stop: the run must fail
            exp['facts']['stop']['rows'][0]['halted'] = True
            exp['facts']['stop']['halted_trials'] = 1
            with open(os.path.join(case, 'expected.json'), 'w') as f:
                json.dump(exp, f)
            result = corpus.run_case(case)
            self.assertEqual(result['status'], 'fail')
            joined = '; '.join(result['errors'])
            self.assertIn('facts.stop.halted_trials', joined)
            self.assertIn('facts.stop.rows[0].halted', joined)

    def test_tampered_slope_range_fails(self):
        with tempfile.TemporaryDirectory(prefix='af_stop_tamper_') as tmp:
            case = os.path.join(tmp, 'card8-stop')
            os.makedirs(case)
            shutil.copy2(CARD8_LOG, os.path.join(case, 'AFSTOP.LOG'))
            with open(os.path.join(CARD8, 'expected.json')) as f:
                exp = json.load(f)
            # a future build with a working stop writes a near-zero slope; the old
            # fixture's locked range must reject that data
            exp['facts']['stop']['rows'][0]['post_stop_slope_pct_s'] = {'min': -1, 'max': 1}
            with open(os.path.join(case, 'expected.json'), 'w') as f:
                json.dump(exp, f)
            result = corpus.run_case(case)
            self.assertEqual(result['status'], 'fail')
            self.assertIn('post_stop_slope_pct_s', '; '.join(result['errors']))


if __name__ == '__main__':
    unittest.main()
