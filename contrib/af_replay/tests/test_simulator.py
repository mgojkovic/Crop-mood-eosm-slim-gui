"""Simulated lens/sensor: physics, determinism, timeout and reversible moves."""
import random
import unittest

import common
from common import af_replay


def _cam(seed=1, x0=0.0, P=None, noise_mult=1.0):
    P = P or af_replay.Params()
    return af_replay.Cam(P, random.Random(seed), common.ideal_pool()[0], x0, noise_mult)


class TestSimulatorDeterminism(unittest.TestCase):
    def test_same_seed_same_trial(self):
        P = af_replay.Params()
        spec = common.ideal_spec(200, seed='det/1')
        first = af_replay.run_trial('B', P, spec)
        second = af_replay.run_trial('B', P, spec)
        self.assertEqual(first, second)

    def test_same_seed_same_monte_carlo_cell(self):
        P = af_replay.Params()
        a = af_replay.run_cell('B', P, ('near', 'mid'), 4, 'mc/1', common.ideal_pool())
        b = af_replay.run_cell('B', P, ('near', 'mid'), 4, 'mc/1', common.ideal_pool())
        self.assertEqual(a, b)
        c = af_replay.run_cell('B', P, ('near', 'mid'), 4, 'mc/2', common.ideal_pool())
        self.assertNotEqual((a['succ'], a['moves'], a['travel']), (c['succ'], c['moves'], c['travel']),
                            'a different seed should give different trials')

    def test_camera_streams_follow_the_seed(self):
        a, b, c = _cam(seed=5), _cam(seed=5), _cam(seed=6)
        pa = [a.measure(1) for _ in range(4)]
        pb = [b.measure(1) for _ in range(4)]
        pc = [c.measure(1) for _ in range(4)]
        self.assertEqual(pa, pb)
        self.assertNotEqual(pa, pc)


class TestSimulatorPhysics(unittest.TestCase):
    def test_curve_peak_is_at_zero(self):
        c = _cam()
        self.assertGreater(c.s_true(0.0), c.s_true(200.0))
        self.assertGreater(c.s_true(0.0), c.s_true(-200.0))
        self.assertAlmostEqual(c.quality(), 1.0)          # x0 == 0 -> at the peak

    def test_same_direction_moves_are_exact(self):
        c = _cam()
        c.last_dir = 1
        c.move(100)
        self.assertEqual(c.x, 100.0)
        c.move(100)
        self.assertEqual(c.x, 200.0)
        self.assertEqual(c.moves, 2)
        self.assertEqual(c.travel, 200.0)

    def test_reversal_offset_is_bounded_by_R(self):
        c = _cam()
        c.last_dir = 1
        c.move(100)
        for seed in range(20):
            c = _cam(seed=seed)
            c.last_dir = 1
            c.move(100)
            c.move(-100)                                  # a reversal: -100 plus an unknown offset
            self.assertLessEqual(abs(c.x), c.P.R, 'reversal offset %.1f exceeds R=%g' % (c.x, c.P.R))

    def test_measurement_noise(self):
        c = _cam(noise_mult=1.0, P=af_replay.Params(cap_ms=1e6))
        c.last_dir = 1
        c.move(300)                                       # away from the peak, on the flank
        vals = [c.measure(1)[0] for _ in range(400)]
        mean = sum(vals) / len(vals)
        sd = (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5
        self.assertAlmostEqual(sd / mean, c.P.noise, delta=0.004)

    def test_continuous_move_and_stop(self):
        c = _cam(x0=200.0)
        h = c.begin(-400)                                 # towards the peak at 0
        seen = []
        while True:
            r = c.next_sample(h)
            if r is None:
                break
            seen.append(r[1])
        self.assertGreater(len(seen), 3)
        self.assertAlmostEqual(c.x, -200.0, delta=1e-9)
        # a stopped move coasts for stop_lat ms only
        c2 = _cam(x0=200.0)
        h2 = c2.begin(400)
        c2.next_sample(h2)
        t_before = c2.t
        c2.stop(h2)
        self.assertGreater(c2.x, 200.0)
        self.assertLess(c2.x, 600.0)
        self.assertGreaterEqual(c2.t - t_before, c2.P.stop_lat - 1e-9)
        self.assertTrue(h2['done'])

    def test_run_cap_raises_timeout(self):
        P = af_replay.Params(cap_ms=100.0)
        c = af_replay.Cam(P, random.Random(1), common.ideal_pool()[0], 0.0)
        with self.assertRaises(af_replay.AFTimeout):
            c.move(1000)

    def test_slow_completion_probability_table(self):
        P = af_replay.Params()
        self.assertLess(af_replay.p_slow_of(P, 20), af_replay.p_slow_of(P, 60))
        self.assertLessEqual(af_replay.p_slow_of(P, 60), af_replay.p_slow_of(P, 1000))
        self.assertEqual(af_replay.p_slow_of(P, 1), P.p_slow[0][1])


if __name__ == '__main__':
    unittest.main()
