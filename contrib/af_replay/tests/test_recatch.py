"""af_recatch: classic B landing vs the patch-11 re-catch landing on the imperfect-lens simulator.

Fixed seeds, small Monte Carlo (about 2 s).  The headline pinned here (see contrib/af_replay/af_recatch.py --extras for the
full 300-1000 trial grid):

  * re-catch is faster and tidier: fewer moves, shorter p90 time, fewer run-cap timeouts, fewer end-stop hits;
  * re-catch does NOT raise the strict success rate (final s >= 90 % of the true peak): over the grid it is on average a
    few points below classic, because it often ends on the far flank of the narrow peak; it does raise the >= 75 % rate
    where the classic landing breaks down (55 mm, bad errors);
  * the classic 55 mm model reproduces the field failure pattern (flat abort -> full resweep -> end stops / timeouts).
"""
import statistics
import unittest

import common  # noqa: F401  (puts contrib/af_replay on sys.path)
import af_recatch as R

N = 120
SEED = 'unit'
_cache = {}


def cell(lens, err, defocus, n=N):
    k = (lens, err, defocus, n)
    if k not in _cache:
        _cache[k] = R.run_cell(lens, err, defocus, n, SEED, ('classic', 'recatch'))
    return _cache[k]


GRID = [(lk, ek, df) for lk in ('55', '18', '50w') for df in (60, 150, 400) for ek in ('realistic', 'bad')]


class TestModel(unittest.TestCase):
    def test_deterministic(self):
        a = R.run_cell('55', 'realistic', 150, 20, 'det', ('classic', 'recatch'))
        b = R.run_cell('55', 'realistic', 150, 20, 'det', ('classic', 'recatch'))
        self.assertEqual(a, b)
        c = R.run_cell('55', 'realistic', 150, 20, 'other-seed', ('classic', 'recatch'))
        self.assertNotEqual(a, c)

    def test_runs_bounded_and_inside_the_range(self):
        for algo in R.ALGOS:
            for ek in ('none', 'realistic', 'bad'):
                for i in range(15):
                    r = R.run_trial(R.LENSES['55'], R.ERRS[ek], 400, algo, SEED, 'bound', i)
                    s = r['sim']
                    self.assertLessEqual(s.travel, R.MAX_TOTAL + 300)
                    self.assertLessEqual(s.t, R.RUN_CAP_B_MS + 1500)
                    self.assertTrue(s.lo - 1e-6 <= s.x <= s.hi + 1e-6)
                    self.assertGreater(s.moves, 3)

    def test_ideal_lens_both_land(self):
        for df in (60, 150):
            m = cell('55', 'none', df)
            self.assertGreaterEqual(m['classic']['ok'], 95)
            self.assertGreaterEqual(m['recatch']['ok'], 85)
            self.assertGreaterEqual(m['recatch']['near'], 98)
            self.assertLess(m['recatch']['t_med'], m['classic']['t_med'])

    def test_sweep_stop_callback(self):
        sim = R.Sim(R.LENSES['55'], R.ERRS['none'], __import__('random').Random(1), 150, __import__('random').Random(2))
        sim.sw_seen, sim.sw_max, sim.sw_below = 0, 100.0, 0
        self.assertFalse(R.Sim.sw_stop(sim, 1, 50.0))     # below 60 % once
        self.assertFalse(R.Sim.sw_stop(sim, 1, 50.0))     # same frame: ignored
        self.assertTrue(R.Sim.sw_stop(sim, 2, 50.0))      # second new frame below 60 %

    def test_classic_fix_delays_flat_abort(self):
        # with the spec's flat_min_travel the retrace cannot abort before 120 pulses (12 steps at fine 10)
        for i in range(60):
            r = R.run_trial(R.LENSES['55'], R.ERRS['bad'], 150, 'classic_fix', SEED, 'fix', i, log=True)
            steps = 0
            for line in r['sim'].log:
                if ' r ' in line and not line.startswith('#'):
                    steps += 1
                if 'retrace k=1' in line and 'flat=1' in line:
                    self.assertGreaterEqual(steps, 12)
                    break


class TestHeadline(unittest.TestCase):
    def test_recatch_is_faster_and_tidier_in_every_error_cell(self):
        for lk, ek, df in GRID:
            m = cell(lk, ek, df)
            c, x = m['classic'], m['recatch']
            tag = '%s/%s/%d' % (lk, ek, df)
            self.assertLess(x['mv_med'], c['mv_med'], tag)
            self.assertLessEqual(x['t_p90'], c['t_p90'] + 0.5, tag)
            self.assertLessEqual(x['why'].get('timeout', 0), c['why'].get('timeout', 0) + 2, tag)

    def test_recatch_does_not_beat_classic_on_strict_success(self):
        """the honest part of the headline: no strict-success gain on average (guard: update the doc if this changes)"""
        diffs = [cell(*k)['recatch']['ok'] - cell(*k)['classic']['ok'] for k in GRID]
        self.assertLess(statistics.mean(diffs), 5.0)
        self.assertLess(min(diffs), -4.0)      # there are cells where re-catch is clearly worse (55 mm narrow, realistic errors)
        worse = [k for k in GRID if cell(*k)['recatch']['ok'] < cell(*k)['classic']['ok'] - 4]
        self.assertTrue(any(k[0] == '55' for k in worse))

    def test_gated_recatch_matches_or_beats_classic(self):
        """patch 11 as built (recatch_gtop: land_top skipped at >= 95 % of the sweep peak, else two-sided) is the
        variant in crop_rec.c: strict success >= classic (small-sample slack) at defocus 150, clearly better on average"""
        diffs = []
        for lk in ('55', '18', '50w'):
            for ek in ('realistic', 'bad'):
                m = R.run_cell(lk, ek, 150, N, SEED, ('classic', 'recatch_gtop'))
                d = m['recatch_gtop']['ok'] - m['classic']['ok']
                diffs.append(d)
                self.assertGreaterEqual(d, -8, (lk, ek, d))
        self.assertGreater(statistics.mean(diffs), 5.0)

    def test_recatch_wins_where_classic_breaks_down(self):
        m = cell('55', 'bad', 150)
        c, x = m['classic'], m['recatch']
        self.assertGreaterEqual(x['near'], c['near'])              # >= 75 % of the peak
        self.assertLess(x['es'], c['es'] * 0.5)                    # end-stop contacts per run
        self.assertLess(x['why'].get('timeout', 0) * 3, max(3, c['why'].get('timeout', 0)))
        m = cell('50w', 'bad', 60)
        self.assertGreaterEqual(m['recatch']['near'], m['classic']['near'])

    def test_classic_55mm_reproduces_the_field_failure_pattern(self):
        """field (Refocus test +-150, 55 mm): about 2 of 5 land, the failures are a flat abort in the landing walk followed by the
        patch-10 full resweep that runs into end stops until the 18 s run cap (139612: 37 moves)"""
        n = 150
        c = cell('55', 'bad', 150, n)['classic']
        self.assertLessEqual(c['ok'], 60)
        self.assertGreaterEqual(c['why'].get('timeout', 0) / float(n), 0.10)
        self.assertGreaterEqual(c['t_p90'], 17.0)
        self.assertGreater(c['es_runs'], 10)
        hits = fails = 0
        for i in range(n):
            r = R.run_trial(R.LENSES['55'], R.ERRS['bad'], 150, 'classic', SEED, '55/bad/150', i, log=True)
            sg = R.field_signature(r['sim'].log)
            if sg['flat'] and sg['resweep'] and not r['ok']:
                hits += 1
                fails += r['moves'] >= 20
        self.assertGreaterEqual(hits / float(n), 0.15)
        self.assertGreaterEqual(fails, hits * 0.5)
        # re-catch never runs the full resweep / other-way jump
        for i in range(40):
            r = R.run_trial(R.LENSES['55'], R.ERRS['bad'], 150, 'recatch', SEED, '55/bad/150', i, log=True)
            self.assertFalse(R.field_signature(r['sim'].log)['resweep'])


P12_N = 300
P12_REALISTIC = [(lk, 'realistic', df) for lk in ('55', '18', '50w') for df in (60, 150, 400)]
_p12 = {}


def p12_cell(k):
    if k not in _p12:
        _p12[k] = R.run_cell(k[0], k[1], k[2], P12_N, SEED, ('recatch_gtop', 'recatch_gtop_l30', 'recatch_gtop_l30_tp', 'recatch_gtop_l30_tpg2'))
    return _p12[k]


class TestPatch12(unittest.TestCase):
    """patch 12 speed changes (S3 tracked probe, S4 short rc chunk + no pad) in the patch-12 time model, vs recatch_gtop.
    The literal S3 (a probe chunk that merely falls counts as a 'peak') is NOT safe: see test_literal_s3_is_unsafe."""

    def _no_worse_and_faster(self, algo):
        for k in P12_REALISTIC:
            m = p12_cell(k)
            b, x = m['recatch_gtop'], m[algo]
            self.assertGreaterEqual(x['ok'], b['ok'] - 5.0, (algo, k, x['ok'], b['ok']))
            self.assertLess(x['t_med'], b['t_med'], (algo, k))

    def test_short_chunk_no_pad_is_no_worse_and_faster(self):
        self._no_worse_and_faster('recatch_gtop_l30')

    def test_guarded_tracked_probe_is_no_worse_and_faster(self):
        self._no_worse_and_faster('recatch_gtop_l30_tpg2')

    def test_tracked_probe_saves_time_where_it_finds_the_peak(self):
        for k in P12_REALISTIC:
            m = p12_cell(k)
            if k[2] <= 150:
                self.assertLess(m['recatch_gtop_l30_tpg2']['t_med'], m['recatch_gtop_l30']['t_med'], k)

    def test_literal_s3_is_unsafe(self):
        """S3 as written: a monotonic fall in the first probe chunk satisfies the sweep 'peak' test, so EF50 wide at defocus 60
        'finds a peak' that is behind it.  Pins the finding; if this fails the spec was fixed in the simulator."""
        m = p12_cell(('50w', 'realistic', 60))
        self.assertLess(m['recatch_gtop_l30_tp']['ok'], m['recatch_gtop']['ok'] - 20)
        self.assertGreater(m['recatch_gtop_l30_tpg2']['ok'], m['recatch_gtop']['ok'] - 5)

    def test_time_model(self):
        sim = R.Sim(R.LENSES['55'], R.ERRS['none'], __import__('random').Random(1), 150, __import__('random').Random(2))
        sim.tm = True
        small = [sim._done_ms(20, False) for _ in range(200)]
        self.assertAlmostEqual(statistics.median(small), 153 + 1.33 * 20, delta=1)
        rev = [sim._done_ms(40, True) for _ in range(400)]
        self.assertGreater(sum(1 for v in rev if v >= 510) / 400.0, 0.85)


if __name__ == '__main__':
    unittest.main()
