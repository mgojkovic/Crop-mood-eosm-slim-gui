"""CLI smoke tests: the commands a developer or a field session actually runs."""
import os
import unittest

import common


class TestAfReplayCli(common.TempDirTestCase):
    def test_selftest(self):
        p = common.run_py(common.AF_REPLAY, '--selftest')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('AFSWEEP parser self-test: ok', p.stdout)

    def test_full_replay_on_synthetic_logs(self):
        d, meta = self.synth_session()
        report = os.path.join(d, 'REPORT.md')
        p = common.run_py(common.AF_REPLAY, '--logs', d, '--trials', 2, '--report', report, '--seed', 'test')
        self.assertEqual(p.returncode, 0, p.stdout[-3000:] + p.stderr[-3000:])
        self.assertIn('logs: %d runs' % meta['afdrive']['runs'], p.stdout)
        self.assertIn('AFSWEEP parser self-test: ok', p.stdout)
        for algo in ('A', 'B', 'C', 'D', 'E', 'F'):
            self.assertIn('%s cold: success' % algo, p.stdout)
        self.assertTrue(os.path.isfile(report))
        with open(report) as f:
            text = f.read()
        for needle in ('# AF replay report', '## 3. Results', '## 4. Recommendation',
                       '### AFSWEEP.LOG', 'AFSWEEP file(s)'):
            self.assertIn(needle, text)

    def test_missing_logs_directory(self):
        d = self.tempdir()
        p = common.run_py(common.AF_REPLAY, '--logs', d)
        self.assertEqual(p.returncode, 2)
        self.assertIn('no AFDRIVE/AFCALIB logs found', p.stdout)

    def test_logs_option_required(self):
        p = common.run_py(common.AF_REPLAY)
        self.assertEqual(p.returncode, 2)
        self.assertIn('--logs is required', p.stderr)


class TestAfReportCli(common.TempDirTestCase):
    def test_healthy_report(self):
        d, meta = self.synth_session(profile='healthy')
        p = common.run_py(common.AF_REPORT, '--logs', d, '--all', '--no-state')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('== AF drive runs: %d, success %d (100%%)'
                      % (meta['afdrive']['runs'], meta['afdrive']['ok']), p.stdout)
        self.assertIn('== Verdict', p.stdout)
        self.assertIn('AF drive OK', p.stdout)

    def test_hunting_report(self):
        d, _ = self.synth_session(profile='hunting')
        p = common.run_py(common.AF_REPORT, '--logs', d, '--all', '--no-state')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('HUNTING', p.stdout)

    def test_no_logs(self):
        d = self.tempdir()
        p = common.run_py(common.AF_REPORT, '--logs', d, '--all', '--no-state')
        self.assertEqual(p.returncode, 0)
        self.assertIn('no *.LOG files found', p.stdout)

    def test_bad_since_argument(self):
        d = self.tempdir()
        with open(os.path.join(d, 'AFDRIVE.LOG'), 'w') as f:   # parse_since runs only with logs present
            f.write('# AFDRIVE t=1 algo=B\n')
        p = common.run_py(common.AF_REPORT, '--logs', d, '--since', 'not-a-time')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('bad --since', p.stderr)


if __name__ == '__main__':
    unittest.main()
