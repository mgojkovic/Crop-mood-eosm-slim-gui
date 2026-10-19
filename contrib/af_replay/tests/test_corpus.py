"""Field-log regression corpus: convention, skip-when-absent and the replay check."""
import json
import os
import shutil
import unittest

import common
import corpus


def _write_expected(case, expected):
    with open(os.path.join(case, 'expected.json'), 'w') as f:
        json.dump(expected, f, indent=2)


class TestCorpus(common.TempDirTestCase):
    def test_committed_corpus_cases(self):
        """Replay every real session in contrib/af_test/corpus/.  With an empty corpus the
        test skips cleanly (no camera, no card, no logs => nothing to regress yet)."""
        cases = corpus.case_dirs()
        if not cases:
            self.skipTest('no field-log corpus cases yet (see contrib/af_test/corpus/README.md)')
        for case in cases:
            result = corpus.run_case(case)
            self.assertEqual(result['status'], 'pass',
                             '%s: %s' % (result['name'], '; '.join(result['errors'])))
        if os.path.basename(os.path.dirname(os.path.dirname(cases[0]))) == 'corpus':
            # the curated 2026-10-19 real-log dataset must stay replayed, not silently disappear
            self.assertGreaterEqual(len([c for c in cases if 'real-20261019' in c]), 4,
                                    'real-20261019 corpus cases are missing')

    def test_empty_corpus_root(self):
        d = self.tempdir()
        summary = corpus.run_all_cases(d)
        self.assertEqual((summary['n'], summary['failed'], summary['skipped']), (0, 0, 0))

    def test_grouped_layout_is_discovered(self):
        """corpus/<group>/<case>/ (e.g. real-20261019) is supported alongside corpus/<case>/."""
        d, meta = self.synth_session()
        root = self.tempdir()
        case = os.path.join(root, '2026-10-19', 'case-a')
        os.makedirs(case)
        for name in meta['files']:
            shutil.copy2(os.path.join(d, name), os.path.join(case, name))
        _write_expected(case, meta['checks'])
        self.assertEqual(corpus.case_dirs(root), [case])
        self.assertEqual(corpus.run_all_cases(root)['passed'], 1)
        # a bare group directory (README only) is not itself a case
        os.remove(os.path.join(case, 'expected.json'))
        for name in meta['files']:
            os.remove(os.path.join(case, name))
        self.assertEqual(corpus.case_dirs(root), [])


REAL = os.path.join(corpus.CORPUS_DIR, 'real-20261019')


class TestReal20261019(common.TempDirTestCase):
    """The curated 2026-10-19 field logs: lock the facts the raw logs contain, independent
    of expected.json (so a re-generated expectation cannot hide a parser regression)."""

    @classmethod
    def setUpClass(cls):
        if not os.path.isdir(REAL):
            raise unittest.SkipTest('real-20261019 corpus dataset is not present')

    def case(self, name):
        path = os.path.join(REAL, name)
        if not os.path.isdir(path):
            self.skipTest('%s is missing' % name)
        return path

    def test_card18_refocus_facts(self):
        facts = corpus.log_facts(self.case('card18-refocus'))
        rt = facts['rtest']
        self.assertEqual(rt['summary'], dict(false_trig=0, ok=3, of=5, final_pct_med=92, runs_total=4))
        self.assertEqual(rt['final_pct'], [82, 92, 83, 96, 98])
        self.assertEqual(rt['ref_s'], [97338, 84320, 70812])
        self.assertTrue(rt['ref_s_monotonic_decrease'])
        self.assertEqual(rt['never_triggered_def'], [400])
        self.assertEqual(rt['triggered_trials'], 4)
        drive = facts['drive']
        self.assertEqual(drive['results'], 7)
        self.assertEqual((drive['fast_moves'], drive['timer_moves']), (51, 6))
        self.assertAlmostEqual(drive['timer_share_pct'], 10.5, delta=0.2)
        self.assertEqual(drive['aref_new_ref'][:7], [101420, 78001, 79418, 80285, 72947, 72466, 73361])
        accepted = drive['local_in_focus'][0]
        self.assertEqual((accepted['s0'], accepted['s'], accepted['last_landing']), (82352, 83863, 101880))
        self.assertAlmostEqual(accepted['pct_of_last_s0'], 80.8, delta=0.2)

    def test_card17_refocus_facts(self):
        facts = corpus.log_facts(self.case('card17-refocus'))
        rt = facts['rtest']
        self.assertEqual(rt['summary'], dict(false_trig=0, ok=2, of=5, final_pct_med=88, runs_total=3))
        self.assertEqual(rt['final_pct'], [88, 95, 77, 91, 87])
        self.assertEqual(rt['ref_s'], [115624, 94755, 85927, 76595])
        self.assertTrue(rt['ref_s_monotonic_decrease'])
        self.assertEqual(rt['never_triggered_def'], [150, 400])
        self.assertEqual(facts['drive']['aborted'], {'0': 5, 'nopeak': 1})
        self.assertEqual((facts['drive']['fast_moves'], facts['drive']['timer_moves']), (42, 6))

    def test_card17_bench_facts(self):
        f = corpus.log_facts(self.case('card17-bench'))['bench']
        self.assertEqual(f['defocus'], [150, -150, 400, -400, 800, -800])
        self.assertEqual(f['algos']['B07']['n'], 6)
        self.assertEqual(f['algos']['B07']['ok'], 6)
        self.assertEqual(f['algos']['B07']['pct'], [103, 100, 100, 101, 100, 108])
        self.assertEqual(f['algos']['B14']['ok'], 4)
        self.assertEqual(f['algos']['B1F']['ok'], 5)
        # the bench's positive-defocus half barely defocuses the scene, its negative half does
        self.assertGreater(f['s_def_pct_positive_min'], 90.0)
        self.assertLess(f['s_def_pct_positive_max'], 115.0)
        self.assertLess(f['s_def_pct_negative_max'], 60.0)

    def test_card8_sweep_facts(self):
        f = corpus.log_facts(self.case('card8-sweep'))['sweep']
        self.assertEqual(f['sweeps'], 16)
        self.assertEqual(f['total_samples'], 183)
        self.assertEqual(f['samples'], [4, 16, 13, 13, 14, 10, 15, 8, 11, 16, 7, 10, 15, 12, 8, 11])
        self.assertEqual(f['cmd'], [600, -600] * 8)
        self.assertEqual(f['speed'], ['7', '7', '1f', '1f'] * 4)
        self.assertEqual(f['timer_mode_sweeps'], 6)
        self.assertGreaterEqual(f['fitted_sweeps'], 8)
        self.assertLess(f['sigma_s_min'], f['sigma_s_median'])
        self.assertLess(f['sigma_s_median'], f['sigma_s_max'])

    def test_post_patch_targets_are_marked_not_current(self):
        for name in ('card18-refocus', 'card17-refocus'):
            case = self.case(name)
            exp = corpus.load_expected(case)
            target = exp.get('post_patch_expectation') or {}
            self.assertTrue(target.get('status', '').startswith('TARGET'),
                            '%s: post_patch_expectation must be marked TARGET' % name)
            self.assertGreaterEqual(target['rtest']['ok_at_least'], 4)
            self.assertTrue(target['rtest']['all_trials_triggered'])
            current = corpus.log_facts(case)['rtest']['summary']
            self.assertLess(current['ok'], target['rtest']['ok_at_least'],
                            '%s: the fixture must remain the pre-patch baseline' % name)

    def test_real_case_regression_is_detected(self):
        """Tamper with a real fixture: changing the expected final_pct sequence must fail."""
        src = self.case('card18-refocus')
        root = self.tempdir()
        case = os.path.join(root, 'tampered')
        shutil.copytree(src, case)
        exp = corpus.load_expected(case)
        exp['facts']['rtest']['final_pct'] = [82, 92, 83, 96, 99]
        _write_expected(case, exp)
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('final_pct' in e for e in result['errors']), result['errors'])

    def test_provenance_hash_is_checked(self):
        src = self.case('card8-sweep')
        root = self.tempdir()
        case = os.path.join(root, 'tampered')
        shutil.copytree(src, case)
        exp = corpus.load_expected(case)
        exp['provenance']['files']['AFSWEEP.LOG']['sha256'] = '0' * 64
        _write_expected(case, exp)
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('provenance' in e for e in result['errors']), result['errors'])

    def test_log_facts_and_report_disagree_is_caught(self):
        src = self.case('card18-refocus')
        root = self.tempdir()
        case = os.path.join(root, 'disagree')
        shutil.copytree(src, case)
        exp = corpus.load_expected(case)
        exp['facts']['rtest']['summary']['ok'] = 5     # log says 3
        _write_expected(case, exp)
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('disagree' in e or 'facts.rtest.summary.ok' in e
                            for e in result['errors']), result['errors'])


class TestCorpusMechanism(common.TempDirTestCase):
    def _synth_case(self, profile='healthy'):
        d, meta = self.synth_session(profile=profile)
        root = self.tempdir()
        case = os.path.join(root, '2026-10-19-synthetic')
        os.makedirs(case)
        for name in meta['files']:
            shutil.copy2(os.path.join(d, name), os.path.join(case, name))
        return root, case, meta

    def test_generated_session_is_itself_a_valid_corpus_case(self):
        """The generator's expected.json has the corpus check keys at the top level, so a
        synthetic session can be dropped into the corpus to exercise the mechanism."""
        d, meta = self.synth_session()
        result = corpus.run_case(d)
        self.assertEqual(result['status'], 'pass', result['errors'])

    def test_case_passes_with_generated_ground_truth(self):
        root, case, meta = self._synth_case()
        _write_expected(case, meta['checks'])
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'pass', result['errors'])
        self.assertEqual(result['metrics']['afdrive']['runs'], meta['afdrive']['runs'])
        self.assertEqual(result['metrics']['rtest']['ok'], meta['rtest']['ok'])

    def test_regression_is_detected(self):
        root, case, meta = self._synth_case(profile='slow')
        checks = json.loads(json.dumps(meta['checks']))
        checks['verdicts']['must_contain'] = ['AF drive OK']     # deliberately wrong: the session is slow
        _write_expected(case, checks)
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('AF drive OK' in e for e in result['errors']), result['errors'])

    def test_case_without_logs_is_skipped(self):
        root = self.tempdir()
        case = os.path.join(root, 'empty-case')
        os.makedirs(case)
        _write_expected(case, {'afdrive': {'min_runs': 1}})
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'skip')

    def test_case_without_expected_json_fails(self):
        root, case, meta = self._synth_case()
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('expected.json' in e for e in result['errors']), result['errors'])

    def test_invalid_expected_json_fails(self):
        root, case, meta = self._synth_case()
        with open(os.path.join(case, 'expected.json'), 'w') as f:
            f.write('{not json')
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('not valid JSON' in e for e in result['errors']), result['errors'])

    def test_replay_curve_check(self):
        root, case, meta = self._synth_case()
        checks = json.loads(json.dumps(meta['checks']))
        checks['replay'] = {'min_curve_fits': 8}
        _write_expected(case, checks)
        self.assertEqual(corpus.run_case(case)['status'], 'pass')
        checks['replay'] = {'min_curve_fits': 1000}
        _write_expected(case, checks)
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('curve_fits' in e for e in result['errors']), result['errors'])

    def test_add_case_script_creates_a_replayable_case(self):
        src, meta = self.synth_session()
        root = self.tempdir()
        p = common.run_py(common.ADD_CASE, '--logs', src, '--name', 'demo', '--root', root)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        case = os.path.join(root, 'demo')
        self.assertTrue(os.path.isfile(os.path.join(case, 'expected.json')))
        self.assertTrue(os.path.isfile(os.path.join(case, 'AFDRIVE.LOG')))
        result = corpus.run_case(case)
        self.assertEqual(result['status'], 'pass', result['errors'])
        summary = corpus.run_all_cases(root)
        self.assertEqual((summary['n'], summary['passed']), (1, 1))


if __name__ == '__main__':
    unittest.main()
