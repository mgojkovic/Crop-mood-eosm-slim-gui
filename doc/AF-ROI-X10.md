# Sharpness ROI and x5 vs x10 AF behaviour

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Camera:** EOS M 2.0.2, EF-M 18-55
STM · **Task:** task-23. Design/analysis only — no camera, no code changes, no commit.
Code references are to `modules/crop_rec/crop_rec.c` at the time of writing; function
names are stable, line numbers drift.

---

## 0. Direct answer to the user's question

**Not the whole frame.** Every sharpness value the AF engine, the Auto-refocus trigger,
the sweep/landing logic, the bench and the `@SHARP` log use is computed from the **centre
25 % × 25 % of the displayed 720×480 LiveView buffer** — `x = width*3/8 .. width*5/8`,
`y = height*3/8 .. height*5/8`, i.e. **180 × 120 display pixels = 6.25 % of the displayed
area** (1/4 in each dimension, 1/16 of the area). It is a Laplacian sum plus a gradient
sum over that single window; there is no whole-frame metric anywhere in the AF code.

At x5 that window is already a magnified, cropped view, so it covers only a small patch of
the scene; at x10 the *same* 180×120 pixel window covers **half the linear scene extent
(one quarter of the area)** of x5. That is one real reason x10 "sees the subject better".
But the likely bigger reason Canon's x10 AF beats Canon's x5 AF on this camera is that
**x5 is the patched crop-rec path and x10 is Canon's unpatched stock path** — crop_rec's
own comments say the x5 patch makes "Canon's AF data … not trustworthy", while x10 is
"left unaltered, for focusing". Magnification and patch state are confounded and the
experiment in §5 is designed to separate them.

---

## 1. Exactly what the ROI is (code facts)

### 1.1 The two implementations

| Function | Used by | Window | Stride | Outputs | Refs |
|---|---|---|---|---|---|
| `afdiag_sharpness_ex()` | `@SHARP` logging, Auto refocus trigger (`aref_eval`), landing/bench decisions, `afd_sample_one` | `x0=3/8·w`, `x1=5/8·w`, `y0=3/8·h`, `y1=5/8·h`, +2 px border | 2 (x and y) | `s = Σ|4c−L−R−U−D|`, `g = Σ(|dx|+|dy|)`, `luma`, `emax`, sample count `n` | `afdiag_sharpness_ex` ~L6115; `afdiag_sharpness` wrapper L6166 |
| `afd_measure()` | every AF engine sample (`afd_move_track`, sweep sampler), sweep-local landing | same ROI but **4 px narrower on each side** (`x0+4 .. x1−4`, same for y) | 4 default, 2 in "full" (sweep-record) mode | `s1`, `g`, `luma`; full mode adds `s2` (neighbour 2 px), `s4` (neighbour 4 px), `band = s1/(s2+s4+1)` | `afd_measure` ~L7835; comment ~L7811; `afd_meas_full` ~L7602 |

Both read `get_yuv422_vram()`, which on the EOS M is a fixed **720×480 UYVY display
buffer** (`vram_lv` in `src/vram.c`), at all zoom levels (`lv_dispsize` 1/5/10). The ROI
therefore stays **180×120 display pixels at every zoom**; only its scene content changes.

Concrete sample counts at 720×480:
- `afdiag_sharpness_ex` (stride 2, 2 px margin): `x` 272…446 step 2 = 88, `y` 182…296 step 2 = 58 → **n ≈ 5 100**;
- `afd_measure` (stride 4, 4 px margin): `x` 274…442 step 4 = 43, `y` 184…292 step 4 = 28 → **n ≈ 1 200**.

`afd_to_ex()` (~L8839) rescales `afd_measure`'s `s1` by the measured/ex sample-count ratio
so landing thresholds can be compared between the two; `afd_ex_n()` (~L8830) is that count.
Both assume a fixed ROI — an ROI selector has to update both (§6).

### 1.2 Every consumer uses this one ROI

- **AF engine samples** — `afd_measure`; all sweeps/local/landing algorithms use `s1`
  (or the rescaled ex value) and `g`.
- **Landing decisions** — `afdiag_sharpness_ex` (`afd_sample_one`, retrace/landing verify).
- **Auto refocus** — `aref_eval` calls `afdiag_sharpness_ex` once per `AREF_PERIOD_MS` and
  drives the peak memory, the trigger, and the "absolute" fallback from this exact window.
- **Bench/sweep records** — same metric, plus `s2/s4/band` in sweep-record mode.
- **Canon's own AF is separate** and uses Canon's AF frame, which is logged from
  `PROP_LV_AFFRAME` (sensor units, `@REFOCUS` w0=8); it is not our ROI.

Consequence: no part of the ML AF system currently measures frame-wide sharpness. A
subject outside the centre 25 % × 25 % (or a background patch inside it) dominates every
decision.

### 1.3 What the ROI covers in the scene

The displayed field is the **active sensor window of the current LV mode**, scaled to
720×480. With the crop preset active (x5), the window is set by the preset; at x10 the
preset is not applied (`is_supported_mode()` returns 0 for `PathDriveMode->zoom == 10`,
`is_supported_mode` ~L1161; the comment "leave the x10 zoom unaltered, for focusing" is at
~L1193), so the displayed field is Canon's stock x10 view.

ROI scale factor: **ROI = 25 % of the displayed width and 25 % of the displayed height**.
If the displayed window is `W_s × H_s` sensor pixels, the ROI is `W_s/4 × H_s/4` and its
share of the sensor is:

| case | displayed sensor window (assumption / code) | ROI in sensor px | ROI width as % of 5184 px sensor | ROI height as % of 3456 px |
|---|---|---|---|---|
| x5, **1:1 crop** preset (slim Mode 1x1 → backend index 1; `crop.preset_1x1=3` = 1440p) | `Preview_H 2552`, `Preview_V 1440` (`CROP_1440p`, ~L3002…3030) | 638 × 360 | 12.3 % | 10.4 % |
| x5, **1x3 anamorphic** preset (the 2026-10-19 AF log headers imply it: `crop_preset=2 slim_mode=1`, and slim Mode UI 1 maps to `CROP_PRESET_1X3`; `Anam_Higher` default = `Preview_H 1372`, `Preview_V 2322`, ~L3308). Horizontal is 3:1 column-binned, so the preview mapping is anisotropic; treat the numbers as indicative until measured | 1372 × 2322 (readout, 3:1 horizontally binned) | 343 × 580 of the preview (343 binned columns ≈1029 sensor columns) | ≈19.8 % of sensor columns (binned) | ≈16.8 % |
| x10, stock (preset off) | ≈ half of the stock x5 window; **assume** ≈1276 × 720 (to be measured) | 319 × 180 | 6.2 % | 5.2 % |

Angular/scene numbers at 18 mm (full-sensor horizontal FOV 63.4°, vertical 45°):

| case | ROI horizontal | ROI width @1 m | ROI width @2 m |
|---|---|---|---|
| x5, 1:1 crop | ≈8.7° | ≈152 mm | ≈305 mm |
| x5, 1x3 anamorphic | anisotropic; depends on how the 3:1-binned columns are mapped to 720 px | must measure (order 100–250 mm) | must measure |
| x10 stock | ≈4.3° | ≈76 mm | ≈152 mm |

So the same 180×120 display-pixel window is, in scene terms, **~2× narrower in each
direction at x10 than at x5** (¼ of the area), for the same preset family. The 1x3 preset
used in the October logs is anisotropic (3:1 column binning) and vertically much taller
than x10, so the exact comparison depends on preset; this is one of the confounds the
experiment must control. Verify the real coverage by putting a ruler at 1 m and measuring
how much of the x5/x10 preview the ROI covers; also log Canon's AF frame via `@REFOCUS`
kind 8.

---

## 2. Why x10 likely behaves better — five candidate causes, ranked

1. **Patch state (likely dominant for Canon AF).** crop_rec's own comment
   (`crop_rec.c` ~L422): *"Crop presets live in the patched x5 path, where Canon's AF data
   is not trustworthy. x10 is left unpatched, so AF is accurate there."* The x5 preset
   changes the sensor readout (RAW width/height, preview geometry, ADTG/CMOS registers)
   while Canon's x5 AF evaluation/area was computed for the unpatched readout. This exactly
   matches the field symptom in `doc/EOSM-AF.md` §3.4: Canon x5 AF "favours far subjects"
   and often fails, while x10 mostly succeeds; the likely cause is an AF-area/geometry
   mismatch, not the lens.
2. **Smaller scene ROI ⇒ subject fills more of the AF window, less background.** At x10
   the AF area/our ROI covers ¼ the scene area of x5. A centred subject therefore occupies
   a larger fraction of the evaluation window, background (often far, high-frequency) is
   excluded, and the classic far-background lock becomes less likely. This is the user's
   intuition and it is real.
3. **More scene detail per display pixel.** x5 under the preset downsamples the sensor
   window heavily (e.g. 2552→720 = 3.5×; 1372→720 = 1.9× horizontally for 1x3); stock x10
   needs only ≈1.8× downsample. Less downsampling preserves higher spatial frequencies:
   the display image is sharper, the Laplacian peak is higher, and for a fixed optical
   defocus (in sensor pixels) the blur covers ≈2× more display pixels. Both effects make
   the contrast/defocus response stronger near focus.
4. **Canon's AF geometry is consistent at x10.** The x10 path is unpatched and Canon's AF
   area is placed where Canon expects it; at x5 crop_rec replaces the readout but Canon
   still computes its AF area from its own x5 assumptions (`doc/EOSM-AF.md` §3.4). This is
   the same point as 1, seen from the AF-area side.
5. **Not optical DOF.** LV zoom does not change lens depth of field (that is set by focal
   length, aperture and distance). What changes is the *measurement scale*: the defocus
   blur is 2× larger in display pixels at x10, so the sharpness curve vs focus pulses is
   expected to be **≈2× narrower in pulses** (measured σ_s ≈ 21 pulses at x5 ⇒ predicted
   ≈10–12 at x10 — to be measured, not assumed). That gives finer landing resolution but a
   smaller search range in pulses; it does not change the lens DOF in pulses.

Caveats that can flip the result:
- **Less texture per ROI.** At x10 the ROI is a small patch: a large, close, low-texture
  subject can fill it with a single flat facet → no peak, `lowtexture` aborts, worse than
  x5. Distant/normal-sized subjects benefit; very close/macro subjects may not.
- **Noise.** The metric ROI has the same number of display samples (n≈1276/5162) at both
  zooms, so *measurement* noise per sample is unchanged; but x10 aggregates fewer sensor
  photosites per display pixel (less spatial averaging), so the display image itself is
  noisier and low light hurts more. AE/exposure differences are a confound.
- **Narrower capture range in pulses.** The x5-tuned constants (§4) become wrong: a
  100-pulse coarse step can jump over the whole x10 peak, and ±40-pulse local probes can
  span several σ.
- **Recording is impossible at x10** (the preset is needed for RAW recording; x10 is a
  non-recording path) — so x10 can only help one-shot/Auto-refocus-not-recording, not the
  recording refocus case.
- **Transition cost/risk.** Going x5→x10 rebuilds LV (~0.3–1 s, existing `slim_zoom_to_x10`
  does it), and it may disturb the open MPU monitor; the camera needs a clean return to x5.

---

## 3. Comparison table

| | x5 + crop preset (current AF work) | x10 stock (preset off) |
|---|---|---|
| Display buffer / metric ROI | 720×480 / 180×120 px, fixed | 720×480 / 180×120 px, fixed |
| Metric cost | ~4 ms (stride 4), full ~24–49 ms | same |
| ROI scene width @18 mm, 1 m | ≈152 mm (1:1 preset); 1x3 preset anisotropic, measure (order 100–250 mm) | ≈76 mm |
| Subject filling / background | less subject, more background | more subject, less background |
| Display downsampling from sensor | ≈3.5× (1:1) / ≈1.9× H (1x3) | ≈1.8× |
| Expected s-curve width in pulses | ≈21 (measured) | ≈10–12 (predicted) |
| Capture range in pulses (g broad peak) | ±~120 | ±~60 |
| Fine-step usefulness | 20 pulses ≈ 1σ | 20 pulses ≈ 2σ (too coarse) |
| Canon AF reliability | poor in field (far-subject bias, failures) | good in field |
| Canon AF path | patched crop-rec x5 readout | unpatched stock |
| Recording | possible (preset needed) | not possible |
| Noise / low-light | better averaging | worse (fewer sensor px per display px) |
| Transition to the other | x5 entry from boot | `set_zoom(10)`, ~0.3–1 s, monitor risk |

---

## 4. Can the MPU engine simply be run at x10?

**Mechanically yes, but not unchanged.** What works:
- Lens moves are MPU commands and are independent of `lv_dispsize` (no zoom term in
  `afd_move_ex`/`afd_goto`); end-stop, FOCUS_DONE, reversal behaviour do not change.
- `afd_measure` reads the same 720×480 display buffer at x10: same ROI in pixels, same
  cost, same `n`. `get_yuv422_vram()` is zoom-independent.
- `afd_engine` itself does not gate on `lv_dispsize` or `CROP_PRESET_MENU`; it only logs
  `zoom=x… crop_preset=…`. A manual **AF drive (MPU) / AF bench / AF sweep record** run
  while the camera is in x10 should therefore execute — to be confirmed on the camera.

What does **not** work unchanged:
- **Auto refocus is explicitly x5-only when not recording**: `aref_blk_mask` sets bit 64
  `if (!(RECORDING ? Auto_refocus == 2 : lv_dispsize == 5))` (~L14148). The Refocus test
  uses the same mask. Running aref at x10 needs a one-line gate change; its reference keys
  (zoom + focal-length bucket) already include zoom, so it will take separate x10
  references correctly.
- **Constants tuned to the x5 curve** (~L7466–7485, 7626):
  `AFD_COARSE 100` on g (x5 peak ±120), `AFD_FINE 20` on s (x5 σ≈21), retrace backs 60/140,
  stop thresholds 70/80/90 %, local probe ±2×fine = ±40, and the absolute `AFD_MIN_G 5000`.
  At x10: coarse 100 ≈ the whole g peak (will skip), fine 20 ≈ 2σ (no fine resolution),
  ±40 local probe spans the peak (can mis-classify "flat"/"in focus"), `AFD_MIN_G` scales
  with n (unchanged n, so numerically OK, but the scene patch is different).
- **Fine step Auto is not a zoom function** (it is focal length based, `afd_fine_eff`
  ~L7541), and the focal length has been unreliable under presets: recent code comment
  says `lens_info.focal_len` stayed 18–21 at 35/55 mm so fine=20 always (~L10481). So no
  automatic x10 scaling exists; it would have to be added or the menu set manually.
- **Monitor/LV state**: x10 entry/exit goes through `set_zoom` and an LV rebuild; whether
  the MPU monitor stays open and whether the first moves are accepted must be tested. The
  existing `slim_zoom_to_x10`/`slim_zoom_from_x10` (L384/L401) already handle the
  transition model for Canon AF and are the safe template.
- **Recording**: cannot use x10 with the preset; this is a one-shot/non-recording path only.
- **The preset changes what "the frame" is**: at x10 the crop preset is not applied, so
  the image the metric sees is Canon's stock x10 view, not the preset's x5 crop. This is a
  feature for separating causes (it is why Canon AF is trustworthy there), but it means
  "x10 AF" is not "same picture, magnified" — it is a different readout.

---

## 5. Options and one-session experiment

### 5.1 Options

**(a) Test the existing engine at x10 — no code (recommended first).**
Enter x10 with SET (default mapping) or the Shutter-zoom setting, then run **AF sweep
record** and **AF bench** from the settings menu (one-shot, not Auto refocus; the aref gate
stays at x5). Measure curve widths, peak contrast, success, time, noise. This answers "does
the metric/engine actually behave better at x10?" with zero build risk. Then optionally
repeat with the Auto-refocus gate temporarily changed (needs a small patch, separate step).

**(b) AF ROI selector (1/4, 1/8, 1/16) or multi-ROI voting.**
Stay at x5 (preset, recording path) and shrink the evaluation window: 1/8 of the displayed
width and height = 90×60 px, which at x5 covers the same scene area as 1/4 does at x10.
This is the low-risk way to get the "less background / more subject" benefit without the
x10 transition, preset loss or recording limitation. Cost: 1/8 has 4× fewer samples ⇒ ~2×
relative reading noise (from the measured ~1 % to ~2 %); recover with stride 2 for the 1/8
ROI (same sample count as today, ~4× the 4 ms measure) or frame averaging. 1/16 (45×30 px,
n≈320 at stride 2) is likely too noisy to use without averaging. A multi-ROI variant
(split the current ROI into 2×2/3×3 cells, pick/texture-weight the cells, or vote across
cells whose peaks agree) keeps full SNR and rejects background patches better than a blind
centre crop; it is more code but no sample loss.
This option does **not** fix Canon's own x5 AF (that is Canon's internal area), only our
ML metric/engine.

**(c) Use x10 only for the final landing/refine.**
Do the coarse search at x5, then switch to x10 for the last fine retrace/landing where the
narrower pulse curve gives better resolution. Cost: one zoom transition (~0.3–1 s) plus a
new LV settle, risk of monitor/LV glitch, and thresholds would need an x10-specific fine
step. Only worth it if (a) shows x10's narrower curve is a real advantage and x5's coarse
search is already good.

**Recommendation:** run (a) together with the Canon comparison in one session. If x10 wins
clearly on the metric and is stable, prefer (c) or full x10 engine for non-recording work
and keep x5 for recording. If x10 is not measurably better but the stock x1 path shows
Canon AF working, the x5 problem is the patch/readout, not the ROI size — then (b) is the
right low-risk improvement for our own metric and Canon x5 remains a Canon-side issue.

### 5.2 One-session experiment: x5 vs x10 (and stock baseline)

Setup: tripod; same static high-contrast chart; 18 mm (then 55 mm); distances 0.5 / 1 / 2 m;
not recording; AF diag log ON; AF drive monitor Keep open; manual exposure fixed
(ISO/shutter/aperture) so luma stays constant; IS off; AF mode (not MF); Auto refocus OFF.
Targets with different texture: detailed chart, low-texture box, high-contrast edge.

Baselines (same scene, same distance, 5 trials each):
1. **x1 stock** (no preset possible: leave preset/mode off or use x1 view — if the slim
   build cannot leave the preset, use the movie-mode x1 before applying it),
   Canon half-press AF; log `@REFOCUS` kind 8 (AF frame), `LV_AF_RESULT`, final sharpness.
2. **x5 + crop preset** (current): Canon half-press AF; our **AF bench** (algorithms B, and
   the local one where available) at −150/−400/+150/+400 pulses; one **sweep record** run;
   one **speed/stop** run if time permits.
3. **x10 stock**: enter x10 (SET / Shutter zoom), Canon half-press AF; our **AF bench**;
   one **sweep record**; then return to x5 and confirm the preset is restored.
Repeat 1–3 at 18 mm/1 m and 18 mm/2 m; then 55 mm if clear differences appear. Optionally
one dim-light run (same exposure) at x10 to test the noise caveat.

Measurements / logs:
- `AFSWEEP.LOG`: fit `s` vs position (the offline tool
  `contrib/af_novel/defocus_estimators.py` already does this) → **σ_s in pulses**, σ_g,
  peak `s`, baseline, reading noise; x5 vs x10 ratio.
- `AFBENCH.LOG`: success %, median time, moves, landing % of reference, `lowtexture` aborts.
- `AFDIAG.LOG`: `@SHARP` static noise (sd/mean), `@LVOBJ`, `@REFOCUS` kind 8 for Canon AF
  frame, `LV_AF_RESULT` and final `@SHARP` after return to x5 for the Canon trials.
- A ruler in the scene at 1 m to measure the ROI/field coverage at x5 and x10 (or use
  `@LV_AFFRAME` coordinates to derive the sensor window ratios).

Acceptance criteria (x10 hypothesis holds if):
- **Metric:** σ_s(x10) / σ_s(x5) ≤ 0.7 (predicted ≈0.5) with static-noise ratio ≤1.5;
  `s_max(x10)` not lower than 0.8× the x5 value on the same scene; x10 static sd/mean
  < 2× x5.
- **Engine:** x10 bench success ≥ 4/4 at ±150 and ±400 pulses, median time ≤1.3× x5, no
  `lowtexture` abort on the detailed chart, and no FOCUS_DONE/monitor/LV failure during the
  x10 run or the return to x5.
- **Canon:** Canon AF success at x10 ≥ at x5 on identical scenes (expected x5 far-locks or
  fails at 1 m where x10 succeeds), and returning to x5 restores the preset and a sharp
  picture.

Falsifiers (stop the x10 route):
- σ_s(x10) not smaller than 0.8× σ_s(x5) — magnification does not sharpen the pulse-domain
  metric; then the only useful change is ROI size (option b), and even that must be tested.
- x10 static noise > 2× x5, or frequent `lowtexture` aborts on the detailed target (loses
  texture/light), or the metric peak is lower (upsampling/processing loses contrast).
- x10 engine fails: moves rejected, monitor closes, LV does not restore, or preset/state
  is broken on return.
- Canon x10 is not better than x5 with the same scene and same AF area logged — the user's
  observation then depends on scene/conditions rather than zoom; compare with the x1 stock
  baseline to see if the x5 preset is the actual culprit.

Interpretation tree:
- x10 metric better + Canon x10 better + x1 stock good → zoom/scene-ROI is the lever; use
  option (c) or x10 engine for non-recording; option (b) for recording.
- x10 metric not better but x1 stock Canon good while x5 Canon poor → the x5 **patch** is
  the problem, not the ROI; do not move the engine to x10 for its own sake; use option (b)
  only if background contamination is seen in the sweep data.
- Neither better → the issue is elsewhere (local-search acceptance, AF area/AF data), not
  zoom/ROI; return to the patch/test workstreams.

---

## 6. If option (b) is chosen: proposed code change (not implemented)

Goal: a Settings → AF diag **"AF ROI"**: `1/4` (default, current behaviour), `1/8`, `1/16`
of the displayed width and height, centred. Keep the same buffer and stride; scale
thresholds and reference keys.

1. **Config/menu:** `static CONFIG_INT("af.roi", afd_roi_menu, 0);` values 0/1/2 →
   denominator `den = 4 << afd_roi_menu` (4, 8, 16). Menu entry next to "Fine step" with a
   help text "Sharpness measurement window (% of display width/height, centred)".
2. **`afdiag_sharpness_ex()`** (~L6115): replace the fixed `x0=3/8·w …` with
   `int half = width / (2*den), x0 = width/2 - half, x1 = width/2 + half;` and likewise
   `y`. Keep the 2 px margins. Same for **`afd_measure()`** (~L7835) with its 4 px margins.
3. **Sample counts:** `afd_ex_n()` (~L8830) and `afd_to_ex()` (~L8839) already derive from
   the ROI sample count; update `afd_ex_n()` to use the same `den` (it is the ex-stride
   count over the ROI). `afd_meas.n` is already stored per sample.
4. **Absolute thresholds:** `AFD_MIN_G 5000` (~L7626) is a sum over the whole ROI, so it
   must scale with `n`: use `min_g = AFD_MIN_G * afd_ex_n() / AFD_EX_N_QUARTER` (or define
   it per-sample). All other local thresholds are percentages (relative) and stay; check
   `AFD_L_*`/`AREF_MIN_G` absolute uses and recalibrate from one sweep session at 1/8.
5. **Reference keys:** ROI must invalidate the peak/reference, like zoom does already.
   Encode it into the existing key: `aref_ref_zoom`/`afd_ref_max_zoom`/`aref_ref_anchor_zoom`
   currently store `lv_dispsize`; store `lv_dispsize | (afd_roi_menu << 8)` and compare
   with the same expression (sites ~L8671, ~L13624, ~L13931, ~L9447). Also clear on ROI
   change in the same place zoom/mm changes clear it.
6. **Logging:** add `roi=%d` (or `den`) to the AFDRIVE/AFRTEST/AFSWEEP headers so a later
   analysis can bucket by ROI; include it in `aref_log()`'s packed word if space allows.
7. **Cost/noise:** n drops 4× at 1/8 and 16× at 1/16 at the same stride. Recommended first
   version: 1/8 with **stride 2** (n≈1 290, same as today's stride-4 count ⇒ ~same noise,
   ~16 ms per measure) and do not offer 1/16 until multi-frame averaging/median exists.
   If stride 2 is too slow, accept ~2× noise at 1/8 stride 4 and widen the local-search
   improve thresholds accordingly.
8. **Calibration session:** one sweep/bench session at 1/8 on the chart, 3 distances ×
   2 focal lengths, to derive the new `AFD_MIN_G`, static noise, and whether the local
   ±2×fine probe still finds a slope; do not ship the selector without it.

**Multi-ROI voting alternative** (more code, better SNR): keep the 1/4 window but split it
into a 2×2 grid (or 3×3); compute `s/g` per cell in the same loop; pick the cell with the
highest stable `g` (texture) or weight cells by `g`; optionally take the median of the
per-cell peaks. This preserves all samples and rejects off-centre background better than a
blind shrink, but needs per-cell calibration because `n` and scene content differ. Defer
until the x5-vs-x10 data shows whether background contamination is actually the problem.

---

## 7. Open/unverified items (to check in the session)

1. Exact x10 stock sensor window and whether x10 is exactly 2× of stock x5 (measure with a
   ruler and `@LV_AFFRAME`; the 720×480 buffer is fixed).
2. Which preset the current AF work actually uses — the recovered October logs show
   `crop_preset=2 slim_mode=1`, i.e. the slim UI's 1x3 anamorphic preset (`Anam_Higher`,
   `Preview_H 1372×2322`), not the 1x1 crop; the latest build may default differently
   (`slim_mode_ui` default 0 = 1x1). Record the preset in the experiment header and note
   that x5-vs-x10 comparisons conflate preset geometry with magnification.
3. Whether the MPU engine's monitor stays open across `set_zoom(10)` and whether the first
   move after the transition is accepted (first run may need the monitor re-open).
4. Whether `AFD_MIN_G`/`AFD_L_*` need rescaling beyond the sample-count factor (the scene
   patch is different at x10, so the texture threshold may hit `lowtexture` more often).
5. Whether Canon's AF frame (`PROP_LV_AFFRAME`) at x5 vs x10 confirms the geometry-mismatch
   hypothesis: compare its w/h/x/y sensor coordinates against the two readout windows.

## 8. Deliverable files

This analysis only: `doc/AF-ROI-X10.md`. No code was changed; the ROI-selector proposal
above is the specification if option (b) is picked.
