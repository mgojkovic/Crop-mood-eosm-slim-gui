"""Smoke tests for contrib/af_test/field_session.py (the one-command post-session checker)."""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TOOL = os.path.join(REPO, "contrib", "af_test", "field_session.py")
CORPUS = os.path.join(REPO, "contrib", "af_test", "corpus", "real-20261019", "card18-refocus")

MODERN_RTEST = """\
# RTEST_BEGIN t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 crop_preset=2 mm=18 fine=20
# RT_REF why=start t=2000 land_s=101159 ref_s=97338 g=147824 luma=104
# RT_TRIAL def=60 s_def=80136 s_def_pct=82 trig_ms=1269 runs=1 run_ms=4517 eng_pct=98 land_s=92408 moves=4 final_s=90697 final_pct=93 settled=1 ok=1 ref_s=97338 no_trigger=0 valid=1 false_pass=0
# RT_TRIAL def=400 s_def=64723 s_def_pct=66 trig_ms=-1 runs=0 run_ms=0 eng_pct=0 land_s=0 moves=0 final_s=69712 final_pct=71 settled=1 ok=0 ref_s=97338 no_trigger=1 valid=0 false_pass=1
# RT_TRIAL_INVALID def=400 reason=no_trigger valid=0 ok=0 false_pass=1
# RTEST false_trig=0 trig_ms_med=1269 ok=1/5 final_pct_med=82 runs_total=1 notrig=1 valid=4 false_pass=1
# RTEST_END aborted=0 pre_trig=0 trials=5 total_ms=150000
"""

HEALTHY_RTEST = """\
# RTEST_BEGIN t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 crop_preset=2 mm=18 fine=20
# RT_REF why=start t=2000 land_s=101159 ref_s=97338 g=147824 luma=104
# RT_TRIAL def=60 s_def=80136 s_def_pct=82 trig_ms=1269 runs=1 run_ms=4517 eng_pct=98 land_s=95408 moves=5 final_s=94697 final_pct=97 settled=1 ok=1 ref_s=97338 no_trigger=0 valid=1 false_pass=0
# RT_TRIAL def=400 s_def=64723 s_def_pct=66 trig_ms=1400 runs=1 run_ms=5200 eng_pct=97 land_s=94500 moves=6 final_s=93600 final_pct=96 settled=1 ok=1 ref_s=97338 no_trigger=0 valid=1 false_pass=0
# RTEST false_trig=0 trig_ms_med=1300 ok=5/5 final_pct_med=96 runs_total=5 notrig=0 valid=5 false_pass=0
# RTEST_END aborted=0 pre_trig=0 trials=5 total_ms=150000
"""

MODERN_DRIVE = """\
# AFDRIVE t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 crop_preset=2
# local s0=82000 plus=84000 minus=81500 dir=0 fine=20 last=96000 ref=115000 trig_pct=82 drop=1
# local: climb still soft on the drop path -> B
# refmax prev=96000 land=93000 now=96000 zoom=5 mm=20 track=1
# aref ref anchor: seed val=115000 em=320 g=150000 zoom=5 mm=20 t=3000
# aref ref anchor: raise val=116000 em=330 g=151000 zoom=5 mm=20 t=9000
# RESULT aborted=0 best_pos=0 best_s=101000 peak_g_pos=0 fine_peak_pos=0 final_s=92408 final_best_pct=96 retraces=1 land_s=92408 land_pct=96 moves=7 n_nodone=0 n_err=0 n_fast=1 total_ms=5200 search_ms=3400 epilogue_ms=1800 end_pos=0 travel=600 algo=L
"""

MODERN_STOP = """\
# AFSTOP t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5
# stop_plan k=1 dir=1 at=150 start_rel=-100 cmd=400 speed=7
# stop_check k=1 t_stop=161 s_stop=21000 s_end=24000 delta_pct=14 exc_pct=16 slope_pct_s=42 halted=0 npost=9 insufficient=0
"""


def run_tool(logs):
    p = subprocess.run([sys.executable, TOOL, "--logs", logs], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


class FieldSessionTest(unittest.TestCase):
    def _case(self, rtest, drive=MODERN_DRIVE, stop=MODERN_STOP):
        d = tempfile.mkdtemp(prefix="af_field_")
        for name, text in (("AFRTEST.LOG", rtest), ("AFDRIVE.LOG", drive), ("AFSTOP.LOG", stop)):
            if text:
                with open(os.path.join(d, name), "w") as f:
                    f.write(text)
        return d

    def test_modern_failures_are_reported(self):
        d = self._case(MODERN_RTEST)
        rc, out = run_tool(d)
        self.assertEqual(rc, 1)
        self.assertIn("notrig=1", out)
        self.assertIn("false pass", out)
        self.assertIn("only 1/5 ok", out)
        self.assertIn("0/1 halted", out)
        self.assertIn("search_ms=3400", out)

    def test_healthy_modern_session_passes(self):
        d = self._case(HEALTHY_RTEST)
        rc, out = run_tool(d)
        self.assertEqual(rc, 0, out)
        self.assertIn("PASS", out)
        self.assertNotIn("FAIL", out)

    def test_old_real_session_is_inconclusive_not_failed(self):
        rc, out = run_tool(CORPUS)
        self.assertEqual(rc, 0, out)
        self.assertIn("pre-patch-2", out)
        self.assertIn("ok=3/5", out)

    def test_profile_defs_params_and_copy(self):
        drive = MODERN_DRIVE + (
            '# PROFILE lens="EF50mm f/1.8 II" mm=50 zoom=1 ok=1 P=90000 W50L=48 W50R=52 W50=50 floor_pct=25 x0_return_pct=97 fine=30 done_word=100 honest_pct=100 done_ms_per10=40 ms=4800\n'
            "# params source=card W50=50 fine=30 probe=200 chunk=100 rc_l1=30 local_probe=60 half=15 trusted=0 floor_abs=0\n")
        rtest = HEALTHY_RTEST.replace("# RT_REF", "# RT_DEFS source=profile W50=50 defs=125,-125,300,-300,750 from=card\n# RT_REF", 1)
        d = self._case(rtest, drive=drive)
        os.makedirs(os.path.join(d, "ML", "SETTINGS"))
        with open(os.path.join(d, "ML", "SETTINGS", "AFLENS.TXT"), "w") as f:
            f.write("v1|EF50mm f/1.8 II|50|1|50|25|100|100|30\n")
        rc, out = run_tool(d)
        self.assertEqual(rc, 0, out)
        self.assertIn("PROFILE lens=\"EF50mm f/1.8 II\"", out)
        self.assertIn("RT_DEFS source=profile W50=50", out)
        self.assertIn("source: card=1", out)
        self.assertIn("AFLENS.TXT: 1 profile(s)", out)


if __name__ == "__main__":
    unittest.main()
