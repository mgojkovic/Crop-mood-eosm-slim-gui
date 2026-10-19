"""Log parsers: AFSWEEP (both formats), AFDRIVE move traces and the curve fit they feed."""
import unittest

import common
from common import af_replay, synth_logs


class TestAfsweepParser(common.TempDirTestCase):
    def test_documented_format_roundtrip(self):
        """synthetic_afsweep -> parse_afsweep -> sweep_series -> fit_gauss recovers peak/sigma."""
        import random
        rng = random.Random(1)
        for cmd, peak in ((600, 300.0), (-600, -250.0)):
            text = af_replay.synthetic_afsweep(rng, cmd=cmd, peak=peak, sig=25.0)
            head, sweeps = af_replay.parse_afsweep(text, text=True)
            self.assertEqual(len(sweeps), 1)
            sw = sweeps[0]
            self.assertEqual(sw['cmd'], cmd)
            self.assertEqual(sw['done']['n'], len(sw['samples']))
            pos, s, g = af_replay.sweep_series(sw, lag_ms=42.0)
            self.assertGreaterEqual(len(pos), 8)
            fit = af_replay.fit_gauss(pos, s)
            self.assertIsNotNone(fit)
            self.assertAlmostEqual(fit['p'], peak, delta=8.0)
            self.assertAlmostEqual(fit['sig'], 25.0, delta=8.0)
            self.assertTrue(af_replay.accept_fit(fit, pos))

    def test_shipped_firmware_format_4_field_samples(self):
        """The shipped crop_rec.c afw_log_samples() writes `S <t_ms> <s1> <g> <luma>`.
        The parser must accept it (regression: an IndexError made --logs crash on a real
        AFSWEEP.LOG)."""
        d = self.tempdir()
        meta = synth_logs.generate(d, seed=7)
        path = d + '/AFSWEEP.LOG'
        with open(path) as f:
            self.assertTrue(any(line.startswith('S ') and len(line.split()) == 5 for line in f),
                            'generator must produce the 4-field shipped format')
        head, sweeps = af_replay.parse_afsweep(path)
        self.assertEqual(len(sweeps), 2)
        self.assertEqual(head.get('zoom'), 'x5')
        for sw, truth in zip(sweeps, meta['afsweep']):
            self.assertEqual(sw['cmd'], truth['cmd'])
            self.assertEqual(len(sw['samples']), truth['samples'])
            pos, s, g = af_replay.sweep_series(sw, lag_ms=42.0)
            self.assertGreaterEqual(len(pos), 8, 'no positions from the 4-field sweep')
            fit = af_replay.fit_gauss(pos, s)
            self.assertIsNotNone(fit)
            self.assertAlmostEqual(fit['p'], truth['peak'], delta=8.0)
            self.assertAlmostEqual(fit['sig'], truth['sigma_s'], delta=8.0)

    def test_short_sample_line_is_ignored(self):
        text = ('# AFSWEEP t=100 zoom=x5\n'
                '# sweep k=1 cmd=600 speed=7 dir=1\n'
                'S 100 20000 25000 110\n'
                'S 142\n'
                'S 500 1 2 3 4\n'          # neither 4 nor 10 fields: ignored, not misparsed
                '# done k=1 t=2000 val=4e0400 n=2\n')
        head, sweeps = af_replay.parse_afsweep(text, text=True)
        self.assertEqual(len(sweeps), 1)
        self.assertEqual(len(sweeps[0]['samples']), 1)


class TestAfdriveParser(common.TempDirTestCase):
    def test_parser_roundtrip_and_curve_recovery(self):
        """Synthetic AFDRIVE.LOG -> load_logs -> same-direction series -> Gaussian fit
        recovers the scene peak and width the generator used."""
        d = self.tempdir()
        meta = synth_logs.generate(d, seed=20261019)
        data = af_replay.load_logs(d)
        self.assertTrue(data['runs'])
        self.assertEqual(len(data['sweeps']), 1)
        self.assertGreaterEqual(len(data['runs'][0]['moves']), 15)
        # every trace line keeps the documented fields
        mv = data['runs'][0]['moves'][0]
        for key in ('ms', 'ph', 'cmd', 'pos', 's', 'g', 'luma', 'done_val', 'flags', 'done_ms'):
            self.assertIn(key, mv)
        fits = af_replay.fit_all_series(data)
        afdrive = [x for x in fits if x['kind'] == 'AFDRIVE']
        self.assertGreaterEqual(len(afdrive), 6, 'expected same-direction passes from the generator')
        peaks = sorted(s['peak'] for s in meta['scenes'])
        sigmas = sorted(s['sigma_s'] for s in meta['scenes'])
        ok = [x for x in afdrive if x['ok_s']]
        self.assertGreaterEqual(len(ok), 6, 'most synthetic passes must be fittable')
        for x in ok:
            p, sig = x['fs']['p'], x['fs']['sig']
            self.assertTrue(any(abs(p - want) <= 12.0 for want in peaks),
                            'fitted peak %.1f not one of the generated peaks %s' % (p, peaks))
            self.assertTrue(any(abs(sig - want) <= 6.0 for want in sigmas),
                            'fitted sigma %.1f not one of the generated widths %s' % (sig, sigmas))

    def test_generator_is_deterministic(self):
        d1 = self.tempdir()
        d2 = self.tempdir()
        synth_logs.generate(d1, seed=99)
        synth_logs.generate(d2, seed=99)
        for name in synth_logs.AF_LOGS:
            with open(d1 + '/' + name, 'rb') as f1, open(d2 + '/' + name, 'rb') as f2:
                self.assertEqual(f1.read(), f2.read(), '%s differs for the same seed' % name)
        d3 = self.tempdir()
        synth_logs.generate(d3, seed=100)
        with open(d1 + '/AFDRIVE.LOG', 'rb') as f1, open(d3 + '/AFDRIVE.LOG', 'rb') as f3:
            self.assertNotEqual(f1.read(), f3.read())

    def test_extract_series_breaks_at_reversal(self):
        d = self.tempdir()
        synth_logs.generate(d, seed=3)
        run = af_replay.load_logs(d)['runs'][0]
        sers = af_replay.extract_series(run)
        self.assertGreaterEqual(len(sers), 2)
        for ser in sers:
            dirs = [af_replay.sign(m) for m in ser['pos'][1:]]
            self.assertTrue(all(d >= 0 for d in dirs) or all(d <= 0 for d in dirs))


if __name__ == '__main__':
    unittest.main()
