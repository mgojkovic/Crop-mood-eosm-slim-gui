"""Shared helpers for the offline AF test suite (stdlib unittest, Python 3 only).

The tests drive the two host tools the project already has:

  contrib/af_replay/af_replay.py  - log parser, curve fit, lens simulator, algorithms A-F
  contrib/af_report/af_report.py  - field-session report and verdicts

and the synthetic session generator in contrib/af_test/synth_logs.py, so no camera
and no card are needed.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AF_REPLAY_DIR = os.path.dirname(HERE)                       # contrib/af_replay
REPO = os.path.dirname(os.path.dirname(AF_REPLAY_DIR))      # repo root
AF_TEST_DIR = os.path.join(REPO, 'contrib', 'af_test')
AF_REPORT_DIR = os.path.join(REPO, 'contrib', 'af_report')
AF_REPLAY = os.path.join(AF_REPLAY_DIR, 'af_replay.py')
AF_REPORT = os.path.join(AF_REPORT_DIR, 'af_report.py')
SYNTH_LOGS = os.path.join(AF_TEST_DIR, 'synth_logs.py')
ADD_CASE = os.path.join(AF_TEST_DIR, 'add_case.py')

for _p in (AF_REPLAY_DIR, AF_TEST_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import af_replay      # noqa: E402
import synth_logs     # noqa: E402


class TempDirTestCase(unittest.TestCase):
    """unittest.TestCase with a per-test temporary directory that is always removed."""

    def tempdir(self):
        d = tempfile.mkdtemp(prefix='af_test_')
        self.addCleanup(shutil.rmtree, d, True)
        return d

    def synth_session(self, profile='healthy', seed=20261019):
        """Generate a synthetic field session; returns (directory, ground truth meta)."""
        d = self.tempdir()
        meta = synth_logs.generate(d, seed=seed, profile=profile)
        return d, meta


def run_py(script, *args, timeout=180, cwd=REPO):
    """Run a python3 script with the current interpreter; returns CompletedProcess."""
    return subprocess.run([sys.executable, script] + [str(a) for a in args],
                          capture_output=True, text=True, timeout=timeout, cwd=cwd)


def ideal_pool():
    """One clean synthetic curve (sigma 21 pulses, as measured at x5 on the 18-55)."""
    return [dict(sig_s=21.0, ab_s=1.5, sig_g=35.0, ab_g=1.2)]


def ideal_spec(x0, seed='ideal', noise_mult=1.0):
    return dict(curve=ideal_pool()[0], x0=float(x0), seed=str(seed), noise_mult=noise_mult)


def load_module(name, path):
    """Import a stand-alone script (e.g. af_report.py) by path and return the module."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
