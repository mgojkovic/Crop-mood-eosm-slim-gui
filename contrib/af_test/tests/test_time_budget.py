"""Tests for contrib/af_test/time_budget.py (field-log wall-clock analyzer)."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import time_budget as tb  # noqa: E402

HDR = '# AFDRIVE t=1000 lens="EF-M18-55mm f/3.5-5.6 IS STM" zoom=x5 crop_preset=2 dist0=31 monitor=KEEP opened_now=0 algo=%s\n'
RUN = HDR + """# focal_mm=21 fine=20 fine_setting=0 landing=%s
# chunk_07=100 chunk_mid=200 chunk_fast=300 unm_speed=1f sweep_speed=7 poll_ms=10
# lv_recover ms=300 luma_pre=40 luma=44 timeout=0
700 i 0 0 20000 30000 40 0 - -1 ret0 dist=31 t=1 wait=0 samp=10 q=500
1300 p 100 100 20000 30000 40 4e0400 - 510 ret0 dist=31 t=1 wait=50 samp=10 q=500
1800 o -100 0 0 0 0 4e0000 - 200 ret0 dist=31 t=0 wait=50 samp=0 q=0
# sweep dir=1 len=100 spd=7 t_send=1800 n=3 peak=0 imax=1 smax=1 smin=1 smax_ex=1 t_pk=0 t_stop=-1 t_done=500 val=4e0400 t_exp=410 t_stable=-1 mspp_x10=29 moved=100 pos_pk=0 over=0 pos=100
S 0 100 200 40
S 40 110 210 40
S 80 120 220 40
# sweep dir=1 len=100 spd=7 t_send=2400 n=3 peak=1 imax=1 smax=1 smin=1 smax_ex=1 t_pk=0 t_stop=-1 t_done=300 val=4e0000 t_exp=410 t_stable=-1 mspp_x10=29 moved=100 pos_pk=0 over=0 pos=200
S 0 100 200 40
S 45 110 210 40
2900 r 20 80 30000 40000 40 4e0000 - 190 ret0 dist=31 t=1 wait=45 samp=12 q=500
%s# epilogue pre_stop_s=1 post_stop_s=1 post_close_s=1 session_close=ON
# RESULT aborted=0 best_pos=0 best_s=1 peak_g_pos=0 fine_peak_pos=0 final_s=1 final_best_pct=95 retraces=1 land_s=30000 land_pct=95 moves=3 n_nodone=0 n_err=0 n_fast=0 total_ms=3000 search_ms=2200 epilogue_ms=1700 end_pos=0 travel=1
"""


def write_session(root, text, name="AFDRIVE.LOG"):
    d = os.path.join(root, "ML", "LOGS")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "w") as f:
        f.write(text)


class TimeBudget(unittest.TestCase):
    def analyse(self, text):
        with tempfile.TemporaryDirectory() as t:
            write_session(t, text)
            return tb.analyse([t])

    def test_every_ms_attributed_once(self):
        runs, _ = self.analyse(RUN % ("B", "classic", ""))
        self.assertEqual(len(runs), 1)
        r = runs[0]
        self.assertEqual(r.wall, 2900 + 1700)
        self.assertEqual(sum(c["ms"] for c in r.cells), r.wall)
        self.assertEqual(r.bsum["session"], 700)
        self.assertEqual(r.bsum["epilogue"], 1700)
        self.assertEqual(r.bsum["b_probe"], 600)
        self.assertEqual(r.bsum["sweep"], 500 + 500 + 100 + 300)   # o setup + chunk1 + gap + chunk2
        self.assertEqual(r.bsum["landing"], 200)

    def test_phase_markers_override_inference(self):
        text = RUN % ("B", "classic", "")
        # the first probe move 'p' and the chunks would be b_probe / sweep by inference; the markers say landing
        text = text.replace("1300 p 100", "# phase name=land t=700\n1300 p 100")
        runs, _ = self.analyse(text)
        r = runs[0]
        self.assertEqual(sum(c["ms"] for c in r.cells), r.wall)
        self.assertEqual(r.bsum["b_probe"], 0)
        self.assertEqual(r.bsum["sweep"], 0)
        self.assertEqual(r.bsum["landing"], 2200)
        runs, _ = self.analyse(RUN % ("B", "classic", ""))
        self.assertEqual(runs[0].bsum["b_probe"], 600)   # no markers: unchanged inference

    def test_chunk_idle_and_frames(self):
        runs, _ = self.analyse(RUN % ("B", "classic", ""))
        r = runs[0]
        self.assertEqual(len(r.ck), 2)
        idle = (r.ck[1]["t_send"] - r.ck[0]["t_send"]) - tb.model_ms(100, "7")
        self.assertAlmostEqual(idle, 600 - 410)
        raw, frm = tb.frame_intervals(runs)["7"]
        self.assertEqual(sorted(raw), [40, 40, 45])

    def test_dedup_and_truncated_copy(self):
        full = RUN % ("L", "classic", "")
        truncated = full[:full.index("2900 r")]
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            write_session(a, truncated)
            write_session(b, full + full.replace("t=1000", "t=5000"))
            runs, meta = tb.load([a, b])
        self.assertEqual(len(runs), 2)
        self.assertTrue(all(r.result_line for r in runs))
        self.assertEqual(meta["dups"], 1)

    def test_classification(self):
        fast = (RUN % ("L", "recatch", "")).replace("# epilogue pre_stop", "# epilogue fast pre_stop")
        runs, _ = self.analyse(fast)
        self.assertEqual(runs[0].cls, dict(type="auto", algo="L", landing="recatch", trig=False))

    def test_local_fallback_detected(self):
        txt = (RUN % ("L", "classic", "")).replace(
            "1300 p", "1200 w 20 20 1 1 40 4e0000 - 100 ret0 dist=31 t=1 wait=1 samp=1 q=1\n# local: wide still flat -> B\n1300 p")
        runs, _ = self.analyse(txt)
        self.assertTrue(runs[0].local_fallback)
        self.assertGreater(runs[0].bsum["local"], 0)

    def test_report_runs(self):
        runs, meta = self.analyse(RUN % ("B", "classic", ""))
        text = tb.report(runs, meta)
        for token in ("PHASE BREAKDOWN", "PER-MOVE COST MODEL", "FRAME INTERVAL", "MEASUREMENT COST", "TOP TIME SINKS"):
            self.assertIn(token, text)


if __name__ == "__main__":
    unittest.main()
