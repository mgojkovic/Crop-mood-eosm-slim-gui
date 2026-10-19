# EOS M ML-driven autofocus: handover (v2 — 2026-10-19 late)

**Date:** 2026-10-19 (late session) · **Branch:** `eosm-slim-gui` · **Camera:** Canon EOS M, fw 2.0.2
**Lens used:** EF-M 18-55 f/3.5-5.6 IS STM (zoom), EF 50 f/1.8 STM on a 0.71× speedbooster (reported as 35 mm)
**Module:** `modules/crop_rec/crop_rec.c` (now ~17.5k lines; the AF block is self-contained)
**Build:** `crop_rec.mo` md5 `f587be4849a959ccb8bbcd54b9d5787f` (patches 1–10), full EOS M zip build clean.

This handover supersedes the morning version. The detailed history and evidence are in
[EOSM-AF-WAY-FORWARD.md](EOSM-AF-WAY-FORWARD.md) (read first), [EOSM-AF.md](EOSM-AF.md) §3.8,
[AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §11 (per-patch behaviour, log lines, rollback),
[AF-HUNT-FOREVER.md](AF-HUNT-FOREVER.md) (persistent hunt design),
[AF-ROI-X10.md](AF-ROI-X10.md) (centre ROI / x5 vs x10), [EOSM-AF-STOP-ANALYSIS.md](EOSM-AF-STOP-ANALYSIS.md),
[EOSM-AF-FOCUSFEED.md](EOSM-AF-FOCUSFEED.md), and the reviews `AF-PATCH-REVIEW*.md`.

---

## 1. Status in one paragraph

ML drives the EF-M lens itself via raw `(9,1,5)` pulses and runs its own contrast AF under crop presets.
**One-shot AF on 18 mm is solid** (5/5 Refocus tests, solid bench), **the 50 mm EF + speedbooster also
passes** (5/5 after the adaptive-defocus retry), **35 mm is borderline** (3/5, one 49-move timeout), and
**55 mm is the open problem**: the B07 reference landing frequently finds no peak or lands soft, and
deep-defocus trials fail even with good light. The persistent never-give-up hunt was added, but the late
field logs showed it was mostly **not attempting** (stale pre-attempt sharpness made it close episodes
as "sharp" immediately); patch 10 fixes that and is **not yet field-validated**. Patch 10 also adds a
good-enough tolerance, a learned fine step (for the 50 mm's too-small steps), a faster epilogue and
55 mm fallbacks. Everything is compile- and host-test-verified, not camera-verified.

---

## 2. Hardware / protocol facts that still hold

- **Actuator:** ICU→MPU `(9,1,5) [hi lo SPD 00 00]` through the packet builder `0x38C4`, inside a
  `(9,0x25,16)` session. SPD `0x07` ≈ 0.35 pulses/ms (measured), `0x1F` ≈ 3× faster for unmeasured
  repositioning. SPD `0xFF` (ML's `lens_focus`) only moves ±1..3 pulses — useless.
- **The MPU monitor must be open.** `MpuMonOpen` = `send(8,3,1,{00})`; without it moves are rejected
  in <40 ms. Opening blacks out LV ~2 s; **Keep open** pays it once.
- **No position feedback.** `focus_dist` does not update and the MPU lens-position cache is stale.
  Every `afd_pos` is commanded-pulse accounting.
- **FOCUS_DONE is not the motion end.** Long moves finish at a ~510 ms timer (bit `0x0400`); moves are
  chunked (100 pulses at 07, 300 at 1F) and the code waits a modelled travel time.
- **`(9,3,1)` stop does NOT halt a running `(9,1)` move.** 12/12 recovered stop trials kept changing
  for 300–500 ms after the stop (see EOSM-AF-STOP-ANALYSIS.md). Do not build stop-on-threshold or
  predictive braking; chunking is the real safety mechanism, and B's position model is biased by this
  (the P1 experiment below).
- **Reversals are fine for short moves** (±40/±80 return to 95–103%); the old "backlash" was moves
  being cut short. The offset distribution R is still not measured.
- **End stop:** the MPU answers further moves <150 ms with bit `0x1000`; two in a row are treated as an
  end stop (`afd_es_check`).
- **Focus measure:** Laplacian sum `s` + gradient sum `g` over the **centre 25 % × 25 %** of the
  720×480 LV buffer (180×120 px = 6.25 % of the frame area; stride 2 in `afdiag_sharpness_ex`, stride 4
  in `afd_measure`). **Not** the whole frame. `g` is broad (±~120 pulses at 18 mm) for coarse work; the
  `s` peak is ~40–60 pulses wide (σ≈21 at 18 mm, x5). Blur floor ≈ 17k ex-scale; sharp subjects 80–180k.
- **Focal length freezes when the MPU monitor opens.** EXIF proves the lens reports 18/35/55 correctly
  (and 35 mm for the 50+speedbooster), but `lens_info.focal_len` and the camera UI stay at the
  pre-open value. Workaround: set the zoom **before** the first AF drive, or use **AF diag → Zoom
  refresh (read lens mm)** (close→read→fine→reopen if the in-place request is stale). Patch 8 adds
  automatic ~1 s detection and `mm_pre=`/`mm_post=` logging; whether the MPU answers while the monitor
  is open is **still the key unmeasured question**.
- **Auto refocus trigger gates** are unchanged from the morning design: relative peak memory, drop
  thresholds 60/75/85 % (sens Low/Medium/High), dwell + 500 ms, absolute q/edge fallback, scene
  settled/steady, failed-scene memory, hold/penalty. Patch 9/10 layer the persistent hunt on top.

---

## 3. Code map (`modules/crop_rec/crop_rec.c`, current tree)

| Area | Function | ~Line | What it does |
|---|---|---|---|
| Engine | `afd_engine` | 10565 | session/monitor/LV wait, dispatch by mode (0 AF drive, 6 bench, 8 Refocus prep, …), epilogue |
| Move layer | `afd_move_ex` | 8199 | chunked move, FOCUS_DONE + travel wait, end stop, logging |
| Tracked move | `afd_move_track` | ~8600 | per-frame sampling during a move + optional stop callback |
| Local search | `afd_run_local` | 9526 | ±2×fine probes, wide ±4×fine on soft/drop, climb, feedback landing; tolerance-aware for AREF |
| B sweep | `afd_run_sweep` / `afd_sweep` | 9285 / 8969 | continuous sweep + stepped feedback landing; learned fine from peak width; full resweep after flat landing |
| Landing | `afd_retrace`, `afd_land_steps` | 8400 / 9135 | retrace 1/2, 90/80 %, walk to best |
| Ref max / keys | `afd_ref_max_*`, `afd_mm_bucket` | ~8750 | per zoom+mm high-water mark, resets (scene/zoom/RAW/toggle/foreign/RT) |
| Engine verify/timing | in `afd_run` | ~9650 | 300 ms landing verify (skipped for AREF with land≥90), fast epilogue for AREF |
| Fine step | `afd_fine_eff`, `afd_fine_learn_from_sw` | ~143 / 8950 | focal-length guess + learned value from B/C sweep peak width |
| Auto refocus | `aref_eval` | 14444 | trigger/gates/absolute fallback; anchor clamp |
| Reference anchor | `aref_mpu_settle`, `aref_mpu_finish` | 14230 / 14721 | post-landing reference adoption, freeze/floor/anchor, settle log |
| Persistent hunt | `aref_hunt_step` + helpers | 15203 | IDLE/ARM/ATTEMPT/JUDGE, never-stop invariant, portfolio, backoff, judge |
| Zoom refresh | `afd_lens_refresh` | 8172 | request fresh PROP_LV_LENS, log `# zoom probe`, manual action + auto poll |
| Tolerance | `aref_tol_pct` | 143 | Strict 92 / Normal 88 / Relaxed 85 |
| Refocus test | `aref_test_run`, `afrt_prep` | ~15800 / 10292 | HOLD/trials, adaptive defocus retry, reference validation/fallback |
| Diagnostics | `afw_run` / bench | 10447 | AFSWEEP/AFSTOP/AFREV/AFBENCH; stop_check |

**Settings worth knowing:**
`AF drive monitor` = Keep open, `AF algorithm` = B, `Sweep speed` = 07, `Session close` = ON,
`Fine step` = Auto (or manual; manual overrides learned), `Refocus method` = MPU drive, `Refocus AF` = Local,
`Auto refocus` = ON/ON+REC, **`Refocus persistence` = Persistent** (default; Normal = patches 1–9.1),
**`Focus tolerance` = Normal 88 %** (default; Strict 92, Relaxed 85). The Refocus test always uses its
own strict 90 % criterion.

---

## 4. Field evidence (what to trust)

### Refocus tests

| Session | Lens / focal | Result | Notes |
|---|---|---|---|
| morning | EF-M 18 mm | 3/5, then **5/5** after reboot | first failure was a near-black scene (luma 18→5), not AF |
| morning | EF-M 35 mm | **5/5** | fine=20 (focal length frozen) |
| morning | EF-M 55 mm | false 5/5 | reference landing 34,269 (soft), trials 90–141 % of it; `false_trig=5` |
| late | EF-M 18 mm | **5/5**, retries=1 | `def0=60→120` retry worked |
| late | EF-M 35 mm | **3/5** | +400 trial timed out after **49 moves / 18.7 s**; −150 took 28 moves / 14.8 s |
| late | EF-M 55 mm | failed | 4 attempts aborted (B07 reference nopeak); extra-light attempt: B07 23,789 → local verify **124,530**, then deep trials failed (+150 at 15 % after 28 moves) |
| late | EF 50 + SB (35 mm) | **5/5**, retries=1 | reference 167,867; but 18–28 moves / 8–12 s: steps too small for this gearing |

### Why auto refocus felt wrong in the late session

`# hunt` logs from that session: **111 episodes, 93 `why=cold`, 75 reset `why=sharp attempts=0 travel=0`
within 60–120 ms, only 19 attempts total.** Cause: `aref_hunt_pre_s` was not cleared between episodes,
so the +10 % improvement rule judged against a stale pre-attempt value and closed the episode as sharp.
**Patch 10 fixes this** (clear `pre_s`, +10 % only after an attempt, 6 s cold cooldown), but it is not
field-validated yet.

### Speed budget (measured, late session successful runs)

- Median wall time **≈9.2 s** = ~0.6 s session open + **6.8 s search** + **1.77 s epilogue**.
- Median **15 moves**, ~**450 ms/move**. Warm 2-move runs: 4.4 s wall (1.75 s is epilogue).
- Patch 10 removes ~1.4 s of epilogue + ~0.3–0.6 s verify for AREF/hunt runs and should reduce moves
  (tolerance + learned step). Expected near-term **30–40 % faster** (median ~5.5–6.5 s; warm ~2.5–3 s);
  deep/failed runs 16–19 s → ~11–14 s, with some converted to successes by the new fallbacks. The next
  speed lever after that is a measured motion model (AFSWEEP) to cut the ~450 ms/move.

### Other confirmed facts

- Stop test: `halted=0` in 4/4 new trials (12/12 total) — stop never halts.
- Zoom refresh works manually (`# zoom refresh done old_mm=.. new_mm=.. fine=..`); automatic detection
  is implemented but waiting for a session where the MPU answers while the monitor is open.
- ROI: centre 25 %×25 %, not whole frame; x10's same ROI covers ~½ the linear scene (¼ area) and x5 is
  more downsampled. The biggest reason Canon x10 behaves better is likely that x5 is the patched crop
  path ("Canon AF data not trustworthy"), then tighter ROI/background rejection. Test the existing
  engine at x10 before changing anything (see AF-ROI-X10.md).

---

## 5. Open problems, in priority order

1. **Validate patch 10 in the field** (P0): does the hunt actually attempt on blurry scenes now? Does
   Normal tolerance 88 % stop false hunting/extra moves? Is the fast epilogue safe? Do 55 mm Refocus
   tests get past the reference landing via `# RT_REF fallback`? Check `# hunt`, `# fine learned`,
   `# epilogue fast`, `# verify skipped`, `# RT_REF fallback`.
2. **55 mm / deep defocus** (P0): B07 reference landing is unreliable and B's flat-landing resweep was
   limited; patch 10 adds a full-B reference fallback and a full-width resweep. If deep trials still
   fail, the next candidates are a full-range coarse scan with 1F, Canon x10 as a coarse finder, or
   x10/ROI (below).
3. **Speed** (P1): after patch 10, measure the remaining per-move ~450 ms. Run AFSWEEP at 07/14/1F to
   calibrate the time→position model, then tune the travel wait/settle. Consider a two-speed search
   (1F coarse, 07 final). Target median ≤5 s.
4. **Adapted/EF lens step size** (P1): the learned fine step from a B/C sweep should fix the 50 mm's
   tiny steps; validate `# fine learned`, and keep the manual Fine step override (40–60) as a fallback.
5. **Focal-length freeze** (P1): manual refresh works; automatic detection only if the MPU answers the
   in-place request. If it does not, either close/reopen (2 s blackout) or accept manual refresh. Do not
   add more focal-length-dependent logic.
6. **x5 vs x10 ROI** (P2): run the existing engine at x10 (AF sweep/bench) before a ROI selector; the
   cheap code option is AF ROI 1/8 at x5 with stride 2 (AF-ROI-X10.md has the spec).
7. **Recording (ON+REC)** (P2): re-test after patch 10's fast epilogue and the patch-9 hunt recording
   rules; watch log completeness (afd_note is 2 KB, flushed after recording) and no monitor ops/tasks.
8. **Safety debt** (P2): engine epilogue is still single-exit by convention; NODONE sends a stop and
   logs; menu task_create is guarded; `is_continuous_af()` is scoped to AREF runs. See
   AF-STATUS-REVIEW.md for the risk register.

---

## 6. Next field session (short)

1. Flash this build; **Movie Servo OFF**; monitor **Keep open**.
2. Settings: Refocus persistence **Persistent**, Focus tolerance **Normal** (try Relaxed later), Fine
   step **Auto**, Refocus AF **Local**, method **MPU drive**, AF diag log **ON**.
3. **Refocus test at 18 mm** — expect 5/5, `retries` 0 or 1, fast epilogue.
4. **55 mm Refocus test** with good light/texture — expect `# RT_REF fallback` instead of an abort; note
   whether deep trials still fail and at which defocus.
5. **Deep-defocus walk**: point at a very blurry subject, then a textured one. Expect real
   `# hunt attempt`/`# hunt outcome` lines now, `# hunt skip why=sharp` / cooldown instead of churn, and
   no hunting after a visibly good picture.
6. **50 mm EF**: run one **AF sweep record** first (learns the fine step) or set Fine step 40–60
   manually, then the walk; check `# fine learned` and move sizes.
7. After: `python3 contrib/af_test/field_session.py --copy` — it prints the Refocus integrity, hunt,
   zoom, timing and gate summaries.

---

## 7. Tooling and build

- Offline suite: `make af-test` or `python3 contrib/af_test/run_all.py` → **84 tests, 4 tiers, 6 real
  corpus cases**, ~14 s. The corpus is in `contrib/af_test/corpus/real-20261019/` (real 2026-10-19 logs,
  sha256 provenance). Add a session with `contrib/af_test/add_case.py`.
- `contrib/af_replay/af_lab.py` + `REPORT_LAB.md`: algorithm ranking on real fitted curves.
- `contrib/af_novel/`: defocus estimators/feed probe with preserved real samples.
- Build: `export PATH=$HOME/gcc-arm-none-eabi-5_4-2016q3/bin:$PATH; make -C platform/EOSM.202 zip
  SHELL=/bin/bash PYTHON=python3`. Gotchas: if the build deletes `modules/crop_rec/module_strings.h`,
  restore the 3-line placeholder; the build regenerates `modules/crop_rec/README.rst` (restore with
  `git checkout --`). Build artifacts are not committed.
- Card flashing: unzip the resulting zip to `/Volumes/EOS_DIGITAL`, verify
  `ML/modules/crop_rec.mo` md5, eject. Backups of every flashed module are in
  `~/af_logs/flashed-backups/`.

---

## 8. Git state

Everything is committed on `eosm-slim-gui` in one commit:
`crop_rec: MPU AF patches 1-10 + offline AF test harness, real corpus and handover`
and pushed to `fork/eosm-slim-gui` (`git@github.com:mgojkovic/Crop-mood-eosm-slim-gui.git`).
The build artifact `crop_rec.mo` md5 is `f587be4849a959ccb8bbcd54b9d5787f`. The upstream `origin`
(`Amit199167/...`) was not pushed to; a PR is the owner's decision. Local field logs are under
`~/af_logs/` (not in the repo); the curated real corpus is in the repo.

---

## 9. Rules for the next engineer (unchanged, plus new)

- **No camera in CI.** All host tests are offline; field validation is manual.
- **Do not tune thresholds on one scene.** Change one algorithm/decision per field run; the logs are the
  ground truth.
- **Never add focal-length-dependent logic** until the monitor freeze is fixed; use the refresh/manual
  Fine step.
- **Respect the safety rules:** no monitor open/close while recording, no `task_create` while recording,
  no x10 while recording, keep the epilogue's stop/session handling, respect abort/deadline/travel caps.
- **Do not build stop-on-threshold / predictive braking** unless a future primitive actually halts a move.
- **Redact/keep logs:** the real corpus is committed; card ROM/MPU dumps and raw session logs stay local.
- **Persistent persistence is a product decision:** Normal is the escape hatch; report false hunting on
  a visibly good picture (it is the top UX risk).
