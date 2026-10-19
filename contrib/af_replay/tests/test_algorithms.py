"""Algorithms A-F on the simulated lens: termination, bounded travel, sane accuracy."""
import statistics
import unittest

import common
from common import af_replay

ALGO_NAMES = [k for k, _, _ in af_replay.ALGOS]


def _specs(cls, n, seed, pool=None):
    """The same trial specs af_replay's Monte Carlo uses, for one defocus class."""
    return af_replay.make_specs(cls, n, seed, pool or [], 'real' if pool else 'synth', 1.0)


class TestAlgorithmsTerminate(unittest.TestCase):
    def test_all_algorithms_terminate_and_stay_bounded(self):
        P = af_replay.Params()
        pool = []                      # synthetic curves, no logs needed
        for name in ALGO_NAMES:
            for cls in ('near', 'mid', 'far', 'warm'):
                for spec in _specs(cls, 2, 'term/%s' % name, pool):
                    res = af_replay.run_trial(name, P, spec)
                    self.assertIsInstance(res, dict)
                    self.assertLessEqual(res['t'], P.cap_ms / 1000.0 + 1e-9,
                                         '%s exceeded the run cap' % name)
                    self.assertGreaterEqual(res['moves'], 1, '%s made no move' % name)
                    self.assertLessEqual(res['moves'], 100, '%s made %d moves' % (name, res['moves']))
                    self.assertGreaterEqual(res['travel'], 0.0)
                    self.assertLessEqual(res['travel'], 8000.0, '%s travelled %.0f pulses' % (name, res['travel']))
                    if res['timeout']:
                        self.assertAlmostEqual(res['t'], P.cap_ms / 1000.0, places=6)

    def test_all_algorithms_survive_high_noise(self):
        P = af_replay.Params()
        for name in ALGO_NAMES:
            for spec in _specs('near', 3, 'noise/%s' % name):
                spec['noise_mult'] = 3.0
                res = af_replay.run_trial(name, P, spec)
                self.assertLessEqual(res['t'], P.cap_ms / 1000.0 + 1e-9)


class TestAlgorithmsAccuracy(unittest.TestCase):
    def test_near_defocus_success_rate(self):
        """With an ideal lens and a near start every algorithm must land in focus most of the
        time; the numbers are deterministic (fixed seeds), the limits leave margin for
        algorithm tuning and Monte Carlo noise."""
        P = af_replay.Params()
        thresholds = dict(A=6, B=8, C=7, D=7, E=6, F=7)
        specs = _specs('near', 10, 'acc/near', common.ideal_pool())
        results = {}
        for name in ALGO_NAMES:
            res = [af_replay.run_trial(name, P, sp) for sp in specs]
            results[name] = res
            ok = sum(r['ok'] for r in res)
            self.assertGreaterEqual(ok, thresholds[name],
                                    '%s only %d/10 in focus near the peak' % (name, ok))
        self.assertGreaterEqual(sum(r['ok'] for res in results.values() for r in res), 44)

    def test_continuous_sweep_is_faster_than_the_baseline(self):
        """B (continuous sweep + stepped retrace) is the recommended structure; on identical
        cold-start trials it must be clearly faster than A (stepped baseline)."""
        P = af_replay.Params()
        specs = _specs('mid', 8, 'speed/ab', common.ideal_pool()) + _specs('far', 8, 'speed/ab', common.ideal_pool())
        A = [af_replay.run_trial('A', P, sp) for sp in specs]
        B = [af_replay.run_trial('B', P, sp) for sp in specs]
        self.assertLess(statistics.median(r['t'] for r in B), statistics.median(r['t'] for r in A),
                        'B should not be slower than A on cold starts')
        self.assertGreaterEqual(100.0 * sum(r['ok'] for r in B) / len(B),
                                100.0 * sum(r['ok'] for r in A) / len(A) - 15.0,
                                'B should be at least as accurate as A (within noise)')

    def test_travel_is_bounded_from_a_far_start(self):
        """No algorithm may wander without limit: travel and move count stay bounded even
        when the subject starts 300-600 pulses away."""
        P = af_replay.Params()
        for name in ALGO_NAMES:
            specs = _specs('far', 2, 'far/%s' % name, common.ideal_pool())
            for spec in specs:
                res = af_replay.run_trial(name, P, spec)
                self.assertLessEqual(res['travel'], P.max_total + 2000.0)
                self.assertLessEqual(res['moves'], 100)


if __name__ == '__main__':
    unittest.main()
