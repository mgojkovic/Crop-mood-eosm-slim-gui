"""Gaussian curve fit: y = B + A*exp(-(p-p*)^2/(2 sigma^2))."""
import math
import random
import unittest

from common import af_replay


def gaussian_series(peak, sigma, step=20.0, n=15, ab=1.5, floor=20000.0, noise=0.0, rng=None):
    pos = [peak - (n // 2) * step + step * i for i in range(n)]
    y = []
    for x in pos:
        v = floor * (1.0 + ab * math.exp(-(x - peak) ** 2 / (2.0 * sigma ** 2)))
        if noise and rng is not None:
            v *= 1.0 + rng.gauss(0.0, noise)
        y.append(v)
    return pos, y


class TestCurveFit(unittest.TestCase):
    def test_noiseless_recovery(self):
        """Peak position and width come back from an ideal Gaussian within the fit grid's resolution."""
        for peak in (-250.0, 0.0, 120.0, 300.0):
            for sigma in (12.0, 18.0, 21.0, 28.0, 40.0):
                pos, y = gaussian_series(peak, sigma)
                fit = af_replay.fit_gauss(pos, y)
                self.assertIsNotNone(fit, 'no fit for peak=%s sigma=%s' % (peak, sigma))
                self.assertLess(abs(fit['p'] - peak), 1.5, 'peak %.2f -> %.2f' % (peak, fit['p']))
                self.assertLess(abs(fit['sig'] - sigma), 1.5, 'sigma %.2f -> %.2f' % (sigma, fit['sig']))
                self.assertGreater(fit['r2'], 0.99)
                self.assertTrue(af_replay.accept_fit(fit, pos))

    def test_noisy_recovery(self):
        rng = random.Random(11)
        for peak, sigma in ((300.0, 21.0), (-250.0, 18.0), (120.0, 28.0)):
            pos, y = gaussian_series(peak, sigma, noise=0.01, rng=rng)
            fit = af_replay.fit_gauss(pos, y)
            self.assertIsNotNone(fit)
            self.assertLess(abs(fit['p'] - peak), 6.0)
            self.assertLess(abs(fit['sig'] - sigma), 6.0)
            self.assertTrue(af_replay.accept_fit(fit, pos))

    def test_flat_and_slope_are_rejected(self):
        pos = [float(20 * i) for i in range(15)]
        flat = [20000.0] * len(pos)
        self.assertIsNone(af_replay.fit_gauss(pos, flat))
        slope = [20000.0 + 500.0 * i for i in range(len(pos))]
        fit = af_replay.fit_gauss(pos, slope)
        self.assertFalse(af_replay.accept_fit(fit, pos), 'a monotonic slope is not a focus curve')

    def test_too_few_points(self):
        pos, y = gaussian_series(0.0, 20.0, n=4)
        self.assertIsNone(af_replay.fit_gauss(pos, y))

    def test_accept_fit_checks_peak_in_range(self):
        pos, y = gaussian_series(0.0, 20.0)
        fit = af_replay.fit_gauss(pos, y)
        self.assertTrue(af_replay.accept_fit(fit, pos))
        shifted = dict(fit, p=fit['p'] + 500.0)   # peak far outside the measured window
        self.assertFalse(af_replay.accept_fit(shifted, pos))
        self.assertFalse(af_replay.accept_fit(None, pos))


if __name__ == '__main__':
    unittest.main()
