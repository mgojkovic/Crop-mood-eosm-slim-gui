"""af_report.py: parsing the synthetic AFDRIVE/AFRTEST/AFBENCH logs and the verdicts."""
import os
import unittest

import common
import corpus

af_report = common.load_module('af_report', common.AF_REPORT)


class TestParseDrive(common.TempDirTestCase):
    def test_generated_session(self):
        d, meta = self.synth_session()
        lines = af_report.lines_of(d + '/AFDRIVE.LOG')
        runs, notes, seg, focal = af_report.parse_drive(lines)
        self.assertEqual(len(runs), meta['afdrive']['runs'])
        self.assertEqual(set(focal), {'18', '35', '50'})
        kinds = [k for k, _ in notes]
        self.assertEqual(kinds.count('trig'), 3)
        self.assertEqual(kinds.count('done'), 3)
        self.assertEqual(kinds.count('hold'), 1)
        self.assertEqual(kinds.count('skip'), 1)
        # LV minutes: from the first event to the last, minus off->on gaps
        lvm = af_report.lv_minutes(seg)
        self.assertAlmostEqual(lvm, (meta['afdrive']['lv_span_ms'] + 900) / 60000.0, delta=0.05)
        # every RESULT run carries the fields the report uses for its verdict
        for r in runs:
            self.assertIn('aborted', r)
            self.assertIn('land_pct', r)
            self.assertIn('total_ms', r)
            self.assertIn('algo', r)

    def test_truncated_aref_line_does_not_crash(self):
        """A card pulled during a write can leave a truncated line; the parser must not die
        (regression: `# aref` alone raised IndexError)."""
        runs, notes, seg, focal = af_report.parse_drive(['# AFDRIVE t=1 algo=B', '# aref', '# aref '])
        self.assertEqual(runs, [])
        self.assertEqual(notes, [])

    def test_lv_minutes_subtracts_off_time(self):
        seg = [[(1000, 'on'), (5000, 'trig'), (11000, 'off'), (13000, 'on'), (17000, None)]]
        # span 16000 ms, minus the 2000 ms off->on gap = 14000 ms
        self.assertAlmostEqual(af_report.lv_minutes(seg), 14000 / 60000.0)


class TestReportText(common.TempDirTestCase):
    def test_healthy_session_sections_and_verdicts(self):
        d, meta = self.synth_session(profile='healthy')
        text = corpus.report_text(d)
        for needle in ('== Card:', '== AF drive runs: %d, success %d (100%%)'
                       % (meta['afdrive']['runs'], meta['afdrive']['ok']),
                       '== Auto refocus: 3 trig, 3 done', '== Bench', '== Refocus test (AFRTEST): 1 summary',
                       'AF drive OK', 'Auto refocus trigger rate OK'):
            self.assertIn(needle, text)
        for bad in ('NOT OK', 'HUNTING', 'CRASH', 'Refocus test: only'):
            self.assertNotIn(bad, text)

    def test_hunting_profile_verdicts(self):
        d, meta = self.synth_session(profile='hunting')
        text = corpus.report_text(d)
        self.assertIn('HUNTING', text)
        self.assertIn('Refocus test: only %d/%d ok' % (meta['rtest']['ok'], meta['rtest']['of']), text)
        self.assertNotIn('AF drive NOT OK', text)

    def test_slow_drive_profile_verdict(self):
        d, _ = self.synth_session(profile='slow')
        text = corpus.report_text(d)
        self.assertIn('AF drive NOT OK', text)
        self.assertNotIn('AF drive OK (', text)

    def test_parse_metrics_matches_ground_truth(self):
        d, meta = self.synth_session(profile='healthy')
        metrics = corpus.parse_metrics(corpus.report_text(d))
        self.assertEqual(metrics['afdrive']['runs'], meta['afdrive']['runs'])
        self.assertEqual(metrics['afdrive']['ok'], meta['afdrive']['ok'])
        self.assertEqual(metrics['afdrive']['median_total_ms'], meta['afdrive']['median_total_ms'])
        self.assertEqual(metrics['rtest']['ok'], meta['rtest']['ok'])
        self.assertEqual(metrics['rtest']['of'], meta['rtest']['of'])
        self.assertEqual(metrics['rtest']['false_trig'], meta['rtest']['false_trig'])
        self.assertEqual(metrics['bench']['B']['ok'], meta['bench']['B']['ok'])
        self.assertEqual(metrics['bench']['B']['n'], meta['bench']['B']['n'])

    def test_verdict_lines_extraction(self):
        text = ('== Verdict\n'
                '   - AF drive OK (100% success, median 3.5 s)\n'
                '   - Auto refocus trigger rate OK (1.35/min)\n')
        self.assertEqual(corpus.verdict_lines(text),
                         ['AF drive OK (100% success, median 3.5 s)',
                          'Auto refocus trigger rate OK (1.35/min)'])


class TestEmptyAndBrokenLogs(common.TempDirTestCase):
    def test_empty_directory_report(self):
        d = self.tempdir()
        p = common.run_py(common.AF_REPORT, '--logs', d, '--all', '--no-state')
        self.assertEqual(p.returncode, 0)
        self.assertIn('no *.LOG files found', p.stdout)

    def test_garbage_log_does_not_crash(self):
        d = self.tempdir()
        with open(d + '/AFDRIVE.LOG', 'w') as f:
            f.write('garbage line without header\n# AFDRIVE t=1 algo=B\n\x00\x01broken\n')
        p = common.run_py(common.AF_REPORT, '--logs', d, '--all', '--no-state')
        self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == '__main__':
    unittest.main()


class TestModernRtestIntegrity(common.TempDirTestCase):
    """Patch 2 appends notrig/valid/false_pass to # RTEST and no_trigger/valid/false_pass
    to # RT_TRIAL.  The report must surface them, keep old logs unchanged, and must not
    truncate the trial lines before the trailing fields (regression: 140-char cut)."""

    def _report(self, rtest_lines):
        d = self.tempdir()
        with open(os.path.join(d, 'AFRTEST.LOG'), 'w') as f:
            f.write('\n'.join(rtest_lines) + '\n')
        return corpus.report_text(d)

    def test_modern_fields_surface_with_verdicts(self):
        text = self._report([
            '# RTEST_BEGIN t=5000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 crop_preset=2 mm=20',
            '# RT_REF why=start t=5100 land_s=97000 ref_s=97000 g=140000 luma=100',
            '# RT_TRIAL def=60 s_def=80000 s_def_pct=82 trig_ms=1100 runs=1 run_ms=4200 eng_pct=98 '
            'land_s=83000 moves=3 final_s=81000 final_pct=83 settled=1 ok=0 ref_s=97000 '
            'no_trigger=0 valid=1 false_pass=0',
            '# RT_TRIAL def=400 s_def=65000 s_def_pct=90 trig_ms=-1 runs=0 run_ms=0 eng_pct=0 '
            'land_s=0 moves=0 final_s=71000 final_pct=96 settled=1 ok=1 ref_s=73000 '
            'no_trigger=1 valid=0 false_pass=1',
            '# RT_TRIAL_INVALID def=400 reason=no_trigger valid=0 ok=0 false_pass=1',
            '# RTEST false_trig=0 trig_ms_med=1100 ok=1/2 final_pct_med=90 runs_total=1 '
            'notrig=1 valid=1 false_pass=1',
            '# RTEST_END aborted=0 pre_trig=0 trials=2 total_ms=90000',
        ])
        # summary carries the new counters and gets a labelled integrity line
        self.assertIn('notrig=1 valid=1 false_pass=1', text)
        self.assertIn('integrity: notrig=1 valid=1/2 false_pass=1', text)
        self.assertIn('1 summary, 3 trial lines (1 invalid)', text)
        # the per-trial integrity fields survive (last field of a ~190-char line)
        self.assertIn('no_trigger=1 valid=0 false_pass=1', text)
        self.assertIn('RT_TRIAL_INVALID def=400 reason=no_trigger', text)
        # verdict explains why a trial was invalid / ok was counted anyway
        self.assertIn('Refocus test: only 1/2 ok', text)
        self.assertIn('Refocus test: 1 trial(s) did not trigger (invalid; valid=1/2)', text)
        self.assertIn('Refocus test: 1 false pass(es)', text)

    def test_clean_modern_log_has_no_integrity_warning(self):
        text = self._report([
            '# RT_TRIAL def=60 s_def=80000 s_def_pct=82 trig_ms=1100 runs=1 run_ms=4200 eng_pct=98 '
            'land_s=83000 moves=3 final_s=81000 final_pct=97 settled=1 ok=1 ref_s=97000 '
            'no_trigger=0 valid=1 false_pass=0',
            '# RTEST false_trig=0 trig_ms_med=1100 ok=1/1 final_pct_med=97 runs_total=1 '
            'notrig=0 valid=1 false_pass=0',
        ])
        self.assertIn('integrity: notrig=0 valid=1/1 false_pass=0', text)
        self.assertNotIn('did not trigger', text)
        self.assertNotIn('false pass', text)

    def test_old_real_logs_keep_working_and_trial_lines_are_not_truncated(self):
        case = os.path.join(corpus.CORPUS_DIR, 'real-20261019', 'card18-refocus')
        if not os.path.isdir(case):
            self.skipTest('real-20261019 corpus dataset is not present')
        text = corpus.report_text(case)
        # old logs have no integrity fields: nothing invented, old verdict intact
        self.assertNotIn('integrity:', text)
        self.assertNotIn('notrig=', text)
        self.assertNotIn('false_pass=', text)
        self.assertIn('Refocus test: only 3/5 ok', text)
        # # RT_TRIAL lines are displayed and run past the old 140-char cut
        trial = [l for l in text.splitlines() if 'RT_TRIAL def=60 ' in l]
        self.assertEqual(len(trial), 1, 'RT_TRIAL lines are not shown in the report')
        self.assertGreater(len(trial[0].strip()), 140)
        self.assertTrue(trial[0].strip().endswith('ref_s=97338'), trial[0].strip()[-60:])
