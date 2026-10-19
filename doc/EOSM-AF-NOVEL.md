# EOS M ML autofocus: out-of-the-box measurement and search ideas

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Camera:** Canon EOS M, fw 2.0.2,
EF-M 18-55 f/3.5-5.6 IS STM · **Module:** `modules/crop_rec/crop_rec.c`
**Status:** research note, no camera attached. Scoring and experiments below are designed
to be run in one field session each.

This note is the "beyond incremental tuning" workstream. It assumes the facts in
[EOSM-AF-HANDOVER.md](EOSM-AF-HANDOVER.md) and the protocol work in
[EOSM-MPU.md](EOSM-MPU.md) / [EOSM-MPU-UART.md](EOSM-MPU-UART.md) /
[EOSM-MPU-ROM.md](EOSM-MPU-ROM.md); it does not re-derive them. Every idea is judged on
whether it is *physically and protocol-plausible on this camera*, not on whether it is
fashionable. Ideas that failed the offline check are kept (and killed) rather than padded.

**New evidence used here.** The field-log copies from 2026-10-19 are still present in the
Claude scratchpad (`card*` directories). Two small artefacts were preserved under
[`contrib/af_novel/samples/`](../contrib/af_novel/samples/): the only real `AFSWEEP.LOG`
(per-frame `s1/s2/s4/g/q/band`) and an extract of the first 12 bytes of every
`(9,0x17,32)` message from the Refocus-test `AFDIAG.LOG`. Everything quantitative in
§3 and §4 comes from those, and can be reproduced with
[`contrib/af_novel/defocus_estimators.py`](../contrib/af_novel/defocus_estimators.py) and
[`contrib/af_novel/focusfeed_probe.py`](../contrib/af_novel/focusfeed_probe.py).

---

## 1. What this camera can and cannot give us

### 1.1 Hard limits (do not design around them)

| Limit | Consequence |
|---|---|
| No lens position during `(9,1,5)` relative moves. `lens_info.focus_pos` is meaningless; the MPU cache `0xFFF8132E` is not refreshed in this mode. | There is no odometry. Every move is open-loop; only sharpness tells us where we are. |
| `(9,3,1)` stop does not halt a `(9,1)` move. | "Stop on threshold" while moving is impossible; only chunked moves with modelled travel time work. |
| `FOCUS_DONE` is not the end of motion (fixed ~510 ms timer mode, early at 0x1F). | Position from command timing is unreliable for large moves. |
| `PROP_LV_FOCUS_DATA` (`0x80050026`) never arrives through ML's property layer on the EOS M. **Zero occurrences** of `@LV_FOCUS_DATA` in the 106 preserved `AFDIAG*.LOG` files, although the underlying `(9,0x17,32)` traffic is continuous. | The Canon AF data feed must be read from the **packet-builder hook**, not from a property handler. This is the most important practical fact in this note. |
| No raw sensor/PDAF readout is exposed to ML tasks; LV gives a processed UYVY display buffer, ~4 ms for the stride-4 centre metric. | Chromatic-aberration and per-channel tricks are not real-time feasible (raw is available only in MLV files, offline). |
| The MPU monitor (`MpuMonOpen`) must be open for moves; opening blacks out LV ~2 s; never while recording. | Any new AF path inherits this constraint. |
| Crop presets work only at x5. | x10 is not available while a preset is active (idea N9/N10 depend on this). |

### 1.2 Assets that are underused today

| Asset | Where | What it could give |
|---|---|---|
| `(9,0x17,32)` per-frame AF data feed, ICU→MPU | `@MPU` hook already captures its **first 12 bytes** at ≤4 Hz; QEMU spell `contrib/qemu/eos/mpu_spells/EOSM.h` has a full 32-byte sample | Canon-computed AF/defocus data serialised every frame (N1). |
| `(9,0x21,61)` AF configuration block | sent once per AF session; first 12 bytes logged all-zero in one log | AF area/mode/search configuration (N10). |
| `s1/s2/s4/g/q/band` per frame in sweep-record mode | `afd_measure(full)`, `AFSWEEP.LOG` | multi-scale defocus signal (N2/N3), calibration (N11). |
| `focus_dist` and the 61-byte `LV_LENS` | updates during **Canon's own** AF (observed: 39→49 cm during x10 half-press), not during our moves | absolute distance prior and scale anchor (N6/N13). |
| end-stop flag (two `0x1000` FOCUS_DONE in <150 ms) | `afd_es_check` | absolute position datum, no sharpness needed (N12/N13). |
| `PROP_LV_AFFRAME`, `@LVOBJ` (Canon LV AF object), `@LV_LENS` raw bytes | logged read-only today | where Canon's AF looks; mode/zoom state; passive position proxies (N15). |
| MLV recording + AF drive (proven once) | `MLV` files + `AFSWEEP.LOG` | full-resolution dataset for offline calibration (N11). |

---

## 2. Method and scoring

Each idea has: **mechanism**, **why it can work on this hardware**, **data/protocol and
whether it exists today**, **effort/risk/impact** (S/M/L and 1–5; risk 5 = can crash or
change lens/camera behaviour), the **cheapest decisive experiment** (one field session
unless stated), and **kill criteria**. Offline-prototyped ideas cite measured numbers.

Priority in the score table: `P = 2*impact − risk − effort_penalty` (S=0, M=1, L=2).
`P` is only a triage score; the shortlist in §4 weights "information gained per session".

---

## 3. Ideas

### N1. Decode the Canon per-frame AF data feed `(9,0x17,32)` — is there a one-step measurement already?

**Mechanism.** On the EOS M the image AF evaluation is computed in the DIGIC/ICU and sent
to the MPU over `(9,0x17,32)` before the MPU drives the lens (the message is in the
ICU→MPU table as `0x80050026` `LV_FOCUS_DATA`). If one of its fields is the
Canon-computed focus difference/defocus for the AF area, the search becomes a decoding
problem: read the field, move by the mapped number of pulses, verify once. If the fields
are only contrast evaluation values, they still give an exposure/zoom-normalised peak
detector that is cheaper and more stable than our Laplacian.

**Why this hardware.** The EOS M is a Hybrid CMOS AF body, and the feed is demonstrably
live: in the Refocus-test log there are 643 `(9,0x17,32)` messages with 637 distinct
payloads over ~3 minutes, i.e. a per-frame feed at the hook's rate limit. The payload
structure is already partly visible from the first 12 logged bytes and the full 32-byte
QEMU spell:

```
byte  0..3   two u16 LE: AF-frame width, height      (idle sentinel 1022 x 510; active 88x44, 26x12, 150x74 at x10)
byte  4..7   u32 "A"                                  field rises as sharpness falls: corr(A, 1/s) = +0.60
byte  8..11  u32 "B"                                  in other logs roughly tracks sharpness: corr(B, 1/s) = -0.37
byte 12..31  unknown (needs full payload logging)
```

The 1022×510 sentinel is not a coincidence: the same payload appears in the QEMU EOSM
spell for `PROP_LV_FOCUS_DATA`, and 700D captures of the same property (`contrib/qemu/eos/mpu_spells/700D.h`)
show the same layout with active frame sizes such as 40×20. Field A vs Laplacian sharpness
is exactly what a Canon-computed defocus magnitude would look like, but the existing log is
confounded by Canon's own zoom/x10 AF activity, so this is a hypothesis, not a result.

**Data/protocol.** Exists today: the `@MPU` hook and `@SHARP` lines (AF diag log = ON + MPU).
Only the **first 12 of 32 bytes** are logged and only at 4 Hz, so step 1 needs no rebuild;
step 2 (all 32 bytes, every frame) needs a small logging change in `crop_rec` owned by the
AF-engine workstream.

**Effort/risk/impact.** S (first session, no build) / **risk 1** (read-only) / **impact 5**.

**Cheapest decisive experiment.** Follow the protocol printed by
`contrib/af_novel/focusfeed_probe.py`: five static chart distances (0.30/0.50/1.0/2.0/5.0 m),
at each one 10 s still + one Canon half-press + six known pulse offsets (−200…+200 at speed
07), at x5 and x10. Look for a field that is monotone in |defocus| **at fixed subject
distance** and does not merely follow the frame w/h. Cost: ~30 min, no build.

**Kill criteria.** A/B unchanged across the six pulse offsets for two scenes, or fully
explained by AF-frame geometry/zoom; or the feed only ticks during Canon's own AF and is
silent during our moves (then the field is Canon-internal state, not a sensor measurement).

**If it works.** This is the single biggest available win: one read + one jump instead of
5–10 moves, and the only route to true one-shot AF on this camera.

---

### N2. Two-position differential multi-band jump (the calibrated coarse estimator)

**Mechanism.** Measure `s1/s2/s4` at the current position `p0`, move a known small `d`,
measure again. The *log ratio of band energies* `r = Δlog(s1/(s2+s4))` cancels the unknown
scene spectrum to first order, and the local slope `r/d` maps to the distance to the focus
plane. Invert a calibrated `K(zoom,aperture)` to get a jump, move, then run one short
verification/parabola step. This is a DFD-style estimator that never needs absolute
position — only known relative moves, which we have.

**Why this hardware.** The cheap metric already computes `s1/s2/s4/g` in full mode
(`afd_measure(full)`, ~24–49 ms, currently only in sweep-record); the normal mode is 4 ms.
One extra move + two measures is ~0.4–0.6 s, cheaper than the 60-pulse coarse climb and
far cheaper than a full run. No new protocol, no monitor changes.

**Does the signal exist?** Yes, and it is the only estimator that survived the offline
cross-scene test. `defocus_estimators.py` fits the real `AFSWEEP` curves, holds one scene
out at a time, and learns a single slope from `Δlog(band)/d` to the focus offset:
**MAE 10–20 pulses at d = 20–40 pulses, 100 % of predictions within 40 pulses**, versus a
do-nothing MAE of ~25 pulses. In contrast, a single-frame absolute `band` is scene-dependent
(within-|defocus| cross-scene spread 135 band units against a near-focus dynamic range of
148), so N2 needs the *differential* form, not an absolute one. The calibration constant
does depend on zoom/aperture and on `sigma_s(pulses)`, so it must be measured at 18/35/50 mm.

**Data/protocol.** Exists: relative moves and full metric. Missing: offline calibration
table `K(zoom)` and an implementation that uses `band` instead of `s` for the jump.

**Effort/risk/impact.** M / **risk 2** (wrong jump costs one extra retrace) / **impact 5**.

**Cheapest decisive experiment.** One session on charts at 0.3/1/2 m and 18/50 mm: from
±50 and ±150 pulses, run pair probes with `d = 20/40/80`, log `r` and the true landing found
by the existing B retrace; fit `K`. Expected signal: `r` monotone and smooth with defocus
out to ±150 pulses, and the predicted jump landing within ±40 pulses of the peak on the
first move.

**Kill criteria.** `r` non-monotone across scenes, or the signal dies within ±60 pulses
(then it cannot replace the coarse stage and is only a peak detector).

---

### N3. Edge-width / PSF-radius estimate for the deep-blur regime

**Mechanism.** Instead of summing edge energy (which is flat far from focus), measure the
*width* of strong edges: find high-gradient pixels, extract the gradient profile across the
edge, and fit the 10–90 % rise distance (or the second moment). For a Gaussian PSF this is
`2.563*sigma_blur`, and it is independent of edge contrast — so it survives exposure and
scene-contrast changes that break the Laplacian. Map `sigma_blur` to `|defocus|` with the
lens blur model and jump.

**Why this hardware.** The LV buffer is available and a sparse edge scan is cheap (a few
ms). The current Laplacian is flat when focus is far off — exactly the regime where
the search wastes moves. This is the classic answer to "there is no gradient far from
focus".

**Does it exist?** The measurement does not; the frame access does. But the offline
synthetic check shows the physics limit: measured width saturates at ≈0.55 px, so below
~0.5 px of blur the estimate is meaningless (at x5 a few focus pulses already produce that
much blur). **It is a coarse estimator only** — useful to seed a jump from deep blur, not
to replace the fine peak.

**Effort/risk/impact.** M / risk 2 / impact 3.

**Cheapest decisive experiment.** Offline first: extend the sweep-record diagnostic with
an edge-width value (crop_rec change, AF workstream) and compare its predictive power to
`s` on the same sweep from deep defocus. Cheap alternative that needs no rebuild: capture
a few LV frames by temporarily logging a raw centre crop to the card (not during
recording), then run the estimator offline. Expected: edge-width monotone with defocus
from ±150 to ±600 pulses, where `s` is flat.

**Kill criteria.** Saturation starts within ±60 pulses of focus (most of the search is
inside that), or the edge detector is too noisy at 1 % luma noise.

---

### N4. Longitudinal dither: two opposite moves, three frames, predicted vertex — **mostly killed**

**Mechanism.** From `p0`, measure `s`, move `+d`, measure, move `-2d`, measure, fit a
parabola (or a Gaussian in `log s`), and jump to its vertex. Two extra moves replace a fine
scan; the fit is scene-scale invariant because only the curve shape is used.

**Why this hardware.** Only `(9,1,5)` moves are needed, which already work, with ±40–80
pulses returning to 95–103 % of focus. It is the cheapest "predict instead of search"
primitive available today.

**Offline result (real curves + measured 1 % noise).** The 3-point vertex is **worse than
doing nothing** for most combinations: at d=10–30 pulses and |p|<30, mean |predicted peak|
is 20–37 pulses versus a do-nothing error of 14.9–17.0 pulses, with up to 40 sign errors
per 480 trials. The second difference amplifies noise; averaging 3 frames per point only partly
fixes it. A coarse *direction* probe (`band` rising to the + side ⇒ focus is on the + side)
does gain about +30 % at |p|<30 with d=5–20, but it is just the existing ±2·fine probe with a
better metric.

**Data/protocol.** Exists; no new protocol.

**Effort/risk/impact.** S / risk 2 / impact 2 (after the offline result).

**Cheapest decisive experiment.** Already done offline. If someone still wants field data,
log the three sample points and the vertex prediction inside the current local search and
compare with the retrace result on the same scene (~20 trials, no new moves).

**Kill criteria.** (Met offline.) Vertex error ≥ current retrace accuracy at measured noise
with ≤3 averaged frames. **Do not implement the vertex; keep only the band-slope direction
probe as part of N2.**

---

### N5. Scene-adaptive ROI / texture map / multi-window voting

**Mechanism.** Replace the fixed centre ¼×¼ with a coarse grid (e.g. 4×4 or 8×6) of `s/g`
computed at stride 8 (~5–10 ms), then (a) select the most textured and temporally stable
window(s) and weight them, or (b) take a robust vote (median of top-k windows) so a
defocused near occluder or a flat centre cannot dominate. Optionally track the chosen window
between frames.

**Why this hardware.** The fixed centre window is a plausible cause of two field symptoms:
Auto refocus triggers on whatever is in the centre, and Canon's own x5 AF "favours far
subjects" (the centre window may contain background when the subject is low/off-centre).
Canon's own AF frame is logged (`PROP_LV_AFFRAME`, `@REFOCUS` kind 8), so we can compare our
ROI with Canon's area. Cost fits a 42 ms frame budget.

**Data/protocol.** All from the LV buffer; no protocol change. A temporary grid log in
sweep-record mode is needed for the offline study (crop_rec change).

**Effort/risk/impact.** M / risk 2 / impact 4.

**Cheapest decisive experiment.** One session, three compositions (subject centred,
off-centre, centred with near occluder) at two distances, sweep-record mode with a 4×4 grid
logged. Offline: do per-window sharpness peaks differ by more than one fine step (>20
pulses)? Does top-k voting land closer to the global best than the centre window? Expected:
>60 pulses difference for the off-centre composition.

**Kill criteria.** Per-window peaks agree within ±20 pulses in most scenes, or the vote is
unstable frame-to-frame (spatial noise).

---

### N6. `focus_dist` as a prior and as a lie detector

**Mechanism.** `focus_dist` is coarse (cm), jittery and does not update during our relative
moves — but it *does* update during Canon's own AF (observed 39→49 cm during an x10
half-press, with `LV_LENS` also changing). Use it (a) as a direction/range prior when the
lens has just been moved by Canon, (b) as a consistency check: if the sharpness peak sits
far from the claimed distance, distrust the reference rather than the picture, and (c) as
the absolute anchor for the pulses↔dioptre calibration in N13.

**Why this hardware.** It is the only absolute distance signal available and it is already
logged in `@REFOCUS`/`@FOCUS_CHG`; the ADC readings cost nothing. A prior can cut the
direction probe and, applied to the local-search bug, can veto "in focus" when the picture
is soft but the claimed distance is far from the previous successful focus distance.

**Data/protocol.** Exists (property + logs).

**Effort/risk/impact.** S / risk 1 / impact 3.

**Cheapest decisive experiment.** One session: Canon half-press at five tape-measured
distances (x5 and x10), 5 trials each; log `focus_dist`, `@LVLENS` bytes and the sharpness
landing. Expected: distance within ±10 % and a monotone `LV_LENS` position field during
Canon AF; if the `pos` field moves monotonically with distance, it becomes a scale anchor.

**Kill criteria.** `focus_dist` never changes in our modes, or is quantised too coarsely
(e.g. only >1 m steps) to distinguish 1 m from 2 m.

---

### N7. Exposure/AGC-invariant normalisation and robust filtering

**Mechanism.** The Laplacian sum scales roughly with contrast² and with exposure. Normalise
by local RMS contrast (already partly done via `q = s/g`), subtract a noise floor estimated
from flat patches, and filter in time with a median-of-3/Kalman on the log metric. The
current code already medians 3 samples for the trigger peak; the gain here is an explicit
noise model and an exposure-invariant landing decision.

**Why this hardware.** Auto refocus compares samples taken seconds apart while Canon's AE
(and during recording, exposure changes) moves the metric. The 1 % per-reading noise is
measured; a filter is cheap and there are static LV frames between moves.

**Data/protocol.** Exists. **Novelty: low** — this is engineering hygiene, not a new
measurement; included because it multiplies the value of N2/N5.

**Effort/risk/impact.** S / risk 1 / impact 3.

**Cheapest decisive experiment.** One session: static scene, step ISO/shutter/aperture while
logging `s/g/q/luma/emax`; measure how far a landing decision moves. Expected: normalised
metric drift <5 % under ±1 EV while raw `s` drifts 30–50 %.

**Kill criteria.** Raw metric drift already <5 % under normal AE changes.

---

### N8. Motion/scene-change detection to stop re-triggering and hunting

**Mechanism.** Cheap row/column luma projections (sums over the LV buffer) give a
frame-to-frame correlation and a global motion estimate. Distinguish panning (coherent
projection shift), scene cut (correlation collapse) and pure defocus (uniform blur increase)
and suppress the refocus trigger during motion, reset the peak on a cut. Track the
focus-plane motion with the projection shift when the camera pans (the plane moves with the
scene).

**Why this hardware.** The existing steady/dwell gates already fixed panning false triggers
(0 false triggers in the last Refocus test), so the remaining value is different: avoid
triggering during deliberate subject motion and avoid re-hunting after a scene cut. This is
a "protect the good result" idea, not a fix for a current failure.

**Data/protocol.** Exists (LV buffer, existing aref gates). The projections are needed by
N5 anyway.

**Effort/risk/impact.** S / risk 1 / impact 2.

**Cheapest decisive experiment.** Replay the preserved decision logs for gate-1 firing
statistics first; only if hunting is visible, one field session with a panning + moving
subject to measure new vs old gate behaviour.

**Kill criteria.** No new hunting/re-trigger case found in logs (then this is dead weight).

---

### N9. Dual-scale x5/x10 strategy

**Mechanism.** x10 magnifies the same lens blur twice as many pixels, so near focus the same
focus error produces a larger metric change per pulse; the `s(p)` curve may be *narrower in
pulses* at x10. Strategy: coarse at x5 (wide monotonic range), fine verify/land at x10, or
use an x10-only measurement for the last ±40 pulses. A second, independent reason: Canon's
own AF at x10 succeeds where x5 often fails (field observation), so if our metric can also
run at x10, the two can be compared directly.

**Why this hardware.** `lv_dispsize` switching and x10 measurement are already used by the
Canon-AF path; `LV_LENS` at x10 is logged. **But crop presets only work at x5**, so this is
only usable when the preset can be dropped (not recording), or if the metric itself is
stable under a brief x10 excursion.

**Data/protocol.** Exists for the metric; requires a way to take x10 measurements under the
preset (untested; might be impossible).

**Effort/risk/impact.** M / risk 3 (zoom transition perturbs LV and Canon state) / impact 3.

**Cheapest decisive experiment.** One session without recording: sweep-record at x5 and x10
on the same chart, compare fitted `sigma_s` in pulses and reading noise. Expected: x10
sigma 1.3–2× narrower. If the preset cannot survive x10, record that and kill.

**Kill criteria.** No measurable narrowing of the pulse-domain curve, or the preset/zoom
state cannot be restored cleanly.

---

### N10. Exploit Canon's own AF: x1 half-press detour, Movie Servo predicate, `(9,0x21,61)` config

Three related routes, in increasing risk:

**(a) x1 Canon-AF detour (recommended first).** Not recording: drop the preset/zoom to x1,
let Canon's Hybrid CMOS AF half-press focus (it works well at x1 — the user's normal
camera AF), then return to x5. Focus position persists across zoom; the detour costs one LV
rebuild (~1–2 s) but replaces our 3–6 s search with Canon's sub-second phase-assisted AF.
Auto refocus currently already uses Canon's x10 AF for the non-recording case, so the code
path exists; the change is the zoom/preset sequence and a risk check on return.

**(b) Movie Servo predicate at x5.** Documented: servo may start only when
`+0xC7C==1 && +0x184==2 && +0x710==1`; at x5 `+0x184==1`, `+0x710==5`. Writing 1 into
`+0x710` at x5 (or patching the predicate at `0xFF18597C`) could let Canon's continuous
phase AF run under crop presets. Untested and risky: Canon uses that value to place the x5
AF area, so the AF area may be wrong; do not try during recording; be ready to power-cycle.

**(c) `(9,0x21,61)` config replay.** The 60-byte AF configuration block is sent once per
AF session. Log the full payload at x1/x5/x10 and photo/movie, diff, identify area/AF-mode
fields, then replay a modified block to move Canon's x5 AF area or enable phase AF. Read
first; write only after the ROM/lens-driver analysis and with a fallback.

**Why this hardware.** The EOS M has Hybrid CMOS AF hardware; using Canon's own phase AF is
the shortest path to reliable focus, and the servo gate is already reverse-engineered in the
ROM. This is the most "out-of-the-box" route that does not require new sensor access.

**Data/protocol.** (a) exists (half-press + zoom + preset control); (b) needs a RAM patch;
(c) needs full logging first.

**Effort/risk/impact.** M / risk 3 for (a), 5 for (b) / impact 4–5.

**Cheapest decisive experiment (a).** One session, 10 subjects at 3 distances: measure Canon
x1 half-press + return-to-x5 sharpness, time, and whether the preset/recording state is
clean. Expected: ≥90 % land in focus, <3 s, no state glitch. (b) is a separate, later,
explicitly opt-in experiment at x1 first.

**Kill criteria (a).** Any preset-restore glitch, focus shift on zoom return, or inability
to return to recording; or no better than current Auto refocus. (b) any Canon assert/crash
or unusable AF area.

---

### N11. MLV raw frames as offline ground truth for a calibrated focus measure

**Mechanism.** Record MLV while running a logged sweep (the REC test already proved AF can
run during RAW recording). Offline, extract raw frames, compute a high-quality full-resolution
focus metric, align frames to the logged `s` samples, and fit the true blur-vs-position
curve. That calibrates N2's `K`, validates the LV metric at native resolution, and provides
the scene-spectrum statistics needed for N3.

**Why this hardware.** MLV is the only access to full-resolution sensor data, and the file is
just copied off the card — no `mlv_lite` accessor is needed. The logged per-frame sweep data
gives the time base.

**Data/protocol.** Exists; needs a small host tool (Python, stdlib) and a recording session.
Risk: dropped frames make alignment harder; the current logs only count vsync cadence.

**Effort/risk/impact.** M / risk 1 / impact 4 (it is the calibration substrate for several
other ideas).

**Cheapest decisive experiment.** One session: 10 s MLV at the recording resolution × two
sweeps per scene × three scenes. Offline: extract 10 frames per sweep, compare native
gradient peak position with the LV `s` peak and the fitted curve width. Expected:
same peak ±20 pulses, and a stable `sigma_s` estimate.

**Kill criteria.** Frame timestamps/alignment unusable, or native and LV metrics disagree on
peak position by more than one fine step (that disagreement would itself be a major finding).

---

### N12. Actuator metrology: end stops and FOCUS_DONE as free position proxies

**Mechanism.** The end stop is an absolute position datum that costs one short move
(two `0x1000` FOCUS_DONE in <150 ms). From a known end stop, perform N uniform
same-direction moves and land by sharpness feedback; compare commanded pulses with the
sharpness peak to measure position error. Detect the *real* motion end from frame-to-frame
sharpness change during a tracked move (instead of FOCUS_DONE, which is often a 510 ms
timer), giving a true speed and a dead-reckoned position between end-stop recalibrations.
This is the only route to approximate odometry without `focus_pos`.

**Why this hardware.** `afd_move_track` already samples every frame during a move, and the
end-stop check already exists. The timing model in the code uses guessed constants; the
measurement is cheap and immediately useful for landings, curve fitting and N14.
Position accuracy needed: the s-curve `sigma_s` is ~21 pulses at x5, so a per-move
standard deviation below ~20 pulses would make dead reckoning useful for one or two moves,
not for long chains.

**Data/protocol.** Exists (`AFSWEEP`, flags, track samples). Needs analysis, not new
protocol.

**Effort/risk/impact.** M / risk 2 / impact 4.

**Cheapest decisive experiment.** One session at 18/35/50 mm: from each end stop, 10×
100-pulse same-direction moves with tracking samples, then a fine scan of the s peak.
Fit pulse→position and its error. Expected: per-move position sd <40 pulses, end-stop
repeatability <20 pulses. If motion-end detection works, replace the guessed travel model.

**Kill criteria.** Per-move sd >50 pulses, or end-stop flags fire only at hard-stop/unstable
times, or dead-reckoning error grows faster than sqrt(n) moves.

---

### N13. Pulses-per-DOF calibration: make step sizes physical

**Mechanism.** Find both end stops → total stroke `S` pulses. A unit-focusing lens satisfies
image extension `x = f²/(d−f)`, so the helicoid position is (to first order) linear in
dioptres: `1/d = 1/f − 1/(f+x)`, and the stroke from 0.25 m to infinity is 4 dioptres.
Hence `pulses/dioptre ≈ S/4`. The depth of field in dioptres is
`Δ(1/d) ≈ 2cN/f²` (c = 0.019 mm for APS-C), so at 18 mm f/3.5 one DOF ≈ 0.41 D ≈ **123
pulses** for S≈1200, while at 50 mm f/5.6 it is ≈0.085 D ≈ **25 pulses** for the same stroke
(a ~5× shrink). This makes "20-pulse fine steps" and the Auto fine-step 20/14/10 interpretable, and explains
the reported worse AF at 30–50 mm: the same step is several DOF wide there. It also gives
N14's deadband in pulses.

**Why this hardware.** Needs only end stops + `focus_dist` (N12/N6), both already available;
the lens law is exact. It turns the biggest open "focal length" problem into a scale
problem, and it costs no new protocol.

**Data/protocol.** Exists; measurement in N12.

**Effort/risk/impact.** S–M / risk 1 / impact 4.

**Cheapest decisive experiment.** One session: stroke pulses at 18/35/50 mm + Canon
half-press at 0.3/1/3 m per focal length; compute pulses/dioptre and compare with the
DOF formula and the observed `sigma_s`. Expected linearity within 15 %, and DOF/pulse
shrinking by ~5× from 18 to 50 mm.

**Kill criteria.** Stroke not repeatable (±10 %), end stops unreachable at 50 mm, or
pulses↔dioptre strongly non-linear (then use a measured lookup table instead).

---

### N14. Warm continuous refocus: deadband + Kalman on the focus plane

**Mechanism.** Maintain a 1-D state (focus-plane position in pulses, velocity) and update it
with the N2 differential measurement or the s-peak tracker. Move only when the predicted
defocus exceeds half the DOF from N13; when the predicted plane moves, follow it with small
moves instead of running a search. This is what "continuous AF" should be: a tracker, not a
scanner.

**Why this hardware.** Moves are relative and cheap when small; the metric runs every frame
at ~4 ms; the DOF deadband is the physical quantity that sets how often to move. No new
protocol. It also protects Auto refocus from the local-search failure mode (the acceptance
threshold becomes physical, not a percentage of a drifting reference).

**Data/protocol.** Exists; needs N2/N13 constants.

**Effort/risk/impact.** M / risk 2 / impact 4 (video), 3 (stills).

**Cheapest decisive experiment.** Replay-first: offline tracker on the preserved `@SHARP`
time series and the `contrib/af_replay` simulator (move count vs deadband, update cost).
Field: one recording session with a slow pan + subject motion, log plane estimate and moves;
compare motion blur/time-in-focus with the current full-run behaviour. Expected: >80 % of
frames within half a DOF with <20 % of the current move count.

**Kill criteria.** Focus-plane velocity too unpredictable (deadband fires every frame), or
the measurement is slower than the DOF crossing time in real scenes.

---

### N15. Information leaks from the lens/MPU: MPU RAM reconnaissance, `LV_LENS` breathing, IS, aperture

**(a) MPU RAM reconnaissance (the most promising leak after N1).** The MPU monitor is open
for AF anyway, and `MpuMonRead` is read-only (`modules/mpu_lab` already calls the ROM
function). Dump a 256–4096-byte window around the lens status/position cache
(`0xFFF81300`…) and the lens-command block (`0xFFF8256C`) before and after known
100-pulse moves and diff. The cached `LensFocusPosition` is documented as *not* refreshed in
this mode, but a raw motor step counter or state word elsewhere in the block may still
increment. If any word tracks commanded pulses (with the right sign and a stable scale),
we have odometry and every idea here gets cheaper.

**(b) Passive `LV_LENS` breathing proxy.** The 61-byte `LV_LENS` payload is logged raw on
change (bytes `0x20–0x2F` in `@LVLENS`), and 2.0.2 itself reads focal length at `0x2C`. If
the lens breathes (focal length changes as it focuses), any slowly varying field could act
as a passive focus-position proxy. Zero build cost: the existing logs already record it.

**(c) IS state.** Optical stabilisation moves the image independently of focus, which is a
direct confound for any sharpness metric. Verify IS is off during ML AF (menu / property),
and gate the search on it. Cheap, low impact.

**(d) Aperture-assisted peak sharpening.** Stopping down widens the focus curve and lowers
peak contrast; opening to max aperture during a search (when exposure allows, never during
recording) gives a sharper peak and better fine-step discrimination, then restore the
recording aperture. Field-check `sigma_s` at f/3.5 vs f/8 on the same chart.

**(e) Killed leaks.** Axial chromatic aberration / per-channel defocus is not accessible in
real time (LV is processed UYVY, chroma subsampled; raw channels only in MLV offline). The
zoom ring is manual, so there is no focus–zoom coupling to exploit. The `(9,0x14)` /
`0x8005001E` focus-position property never arrives on the EOS M, and there is no outgoing
request that would make it arrive — do not spend time on it beyond (a).

**Effort/risk/impact.** (a) M / risk 1 (read-only) / impact 5 if a counter is found;
(b) S / risk 1 / impact 3; (c) S / risk 1 / impact 2; (d) S / risk 2 / impact 2.

**Cheapest decisive experiment.** (a) same session as N12: `MpuMonRead` a 1 KB window
before/after each of 10 known moves; diff. Expected: a word changing by the commanded
pulse count. Cost: ~0 extra, the monitor is already open. (b) read the existing `@LVLENS`
lines (2 in one log) — but need a session with several moves to see whether any byte
changes; (d) two sweeps at two apertures, offline.

**Kill criteria.** (a) no word tracks pulses, or only the known-stale cache changes, or
monitor reads perturb the motion timing so much that they cannot be used during a move;
(b) no byte correlates with focus; (d) no measurable `sigma_s` difference.

---

## 4. Score table and shortlist

Effort S/M/L; Risk 1–5 (5 = can hang/crash the camera); Impact 1–5 for *this* project
(Auto refocus quality and speed). `P = 2·I − R − e` (e = 0/1/2 for S/M/L).

| # | Idea | E | R | I | P | Verdict |
|---|---|---|---|---|---|---|
| N1 | Decode Canon `(9,0x17,32)` AF feed | S (decode: M) | 1 | 5 | **9** | **Shortlist #1** |
| N2 | Two-position differential multi-band jump | M | 2 | 5 | **7** | **Shortlist #2** |
| N3 | Edge-width / PSF radius (coarse only) | M | 2 | 3 | 3 | Parked; supports N2 from deep blur |
| N4 | 3-point dither vertex | S | 2 | 2 | 2 | **Killed by offline test** (keep band direction probe) |
| N5 | Adaptive multi-window ROI / voting | M | 2 | 4 | 5 | Strong second wave; needs grid log |
| N6 | `focus_dist` prior + lie detector | S | 1 | 3 | 5 | Do with N13; cheap |
| N7 | Exposure-invariant normalisation | S | 1 | 3 | 5 | Hygiene; multiplies N2/N5 |
| N8 | Motion/scene-change suppression | S | 1 | 2 | 3 | Only if hunting reappears |
| N9 | x5/x10 dual scale | M | 3 | 3 | 2 | Try only if x10 works under preset |
| N10 | Canon AF exploitation (x1 detour / servo / config) | M | 3–5 | 4–5 | 4–5 | **(a) shortlist #3**; (b) high risk |
| N11 | MLV raw ground truth | M | 1 | 4 | 6 | Enabler; good second-wave |
| N12 | End-stop / FOCUS_DONE position proxies | M | 2 | 4 | 5 | Do with N13/N15a in one session |
| N13 | Pulses-per-DOF calibration | S–M | 1 | 4 | 6 | Do with N12/N6 |
| N14 | Warm deadband + Kalman refocus | M | 2 | 4 | 5 | After N2/N13 constants |
| N15a | MPU RAM position-counter hunt | M | 1 | 5 | 8 | Best second information leak |
| N15b–d | Breathing / IS / aperture leaks | S | 1–2 | 2–3 | 2–5 | Cheap add-ons |

### Shortlist: top 3, one session each

**#1 — N1: decode the Canon AF feed (no build, ~30 min).**
Rationale: it is the only idea that could turn search into one-step measurement, and the
data already flows through an existing hook. Experiment: the five-distance, six-pulse-offset,
x5/x10 protocol in `focusfeed_probe.py`; look for a field monotone in |defocus| at fixed
scene. Cost: one session, no build. Decisive either way: a positive answer re-orders the
whole roadmap; a negative answer kills the "Canon computes it" branch and sends effort to N2.

**#2 — N2: two-position differential band jump (one build + one session).**
Rationale: the only non-Canon estimator that already passes a cross-scene offline test
(MAE 10–20 pulses, 100 % within 40 pulses at d=20–40). It reuses the full metric that
already exists and replaces the coarse climb with one move. Experiment: pair probes as in
§N2 on charts at 18/50 mm, fit `K`, then let it make the first jump and measure the landing.
Cost: one small algorithm patch + one session, 20–30 trials.

**#3 — N10a: Canon x1 half-press detour for non-recording Auto refocus (one session).**
Rationale: highest practical payoff for the actual open problem (Auto refocus 3/5), uses
Canon's Hybrid CMOS AF where it is strongest, and reuses the existing half-press/AF-request
code. Experiment: 10 subjects × 3 distances, compare time and return-to-x5 sharpness with the
current local search. Cost: one session, no lens risk; if the preset restore is glitchy, kill.

**Run in the same session (free add-ons):** N12 end-stop stroke + N13 pulses/DOF + N6
`focus_dist` + N15a MPU-RAM diff. All four need only the monitor that is already open, a
tape measure and a chart, and they answer the "where is the lens / how big is a step"
questions that every other idea depends on.

**Single most promising information leak to investigate first:** N1 — `(9,0x17,32)`.
Second: N15a — an MPU RAM motor-step counter via `MpuMonRead`.

---

## 5. Offline prototype results (reproducible)

`python3 contrib/af_novel/defocus_estimators.py` (real `samples/AFSWEEP.LOG`, 15 sweeps,
6 with a usable inside-peak fit; positions normalised to `sigma_s = 21` pulses):

- **Single-frame absolute `band` does not generalise.** At a fixed |defocus| the cross-scene
  spread is ~135 band units, comparable to the whole near-focus dynamic range (~148). This
  kills "one frame gives absolute blur" in its naive form.
- **Two-position differential `Δlog(band)` does generalise** in leave-one-scene-out
  validation: MAE 10.0 / 18.4 / 20.1 pulses and 100 % within 40 pulses for d=20/30/40;
  do-nothing MAE 23–27. Adding an absolute-band feature makes it worse (overfits scene),
  so the local-difference form is the right one.
- **3-point dither vertex is worse than doing nothing** at the measured 1 % reading noise
  (mean |predicted peak| 17–37 pulses inside |p|<30 vs do-nothing 14–17; tens of sign
  errors). Averaging 3 frames per point only partly recovers it. This is why N4 is killed.
- **Edge-width/PSF saturates** below ~0.5 px of blur (synthetic 10–90 % rise estimator:
  bias <0.15 px above 0.8 px blur, but pinned at 0.39–0.55 px for true 0.2–0.3 px). Coarse
  estimator only.

Limitations to state in any write-up: the position scale assumes `sigma_s = 21` pulses
(measured at x5/18 mm); the sweep file is a concatenation of sessions with no per-sweep
zoom header; the differential calibration in N2 still has to be measured on real charts.
The prototype is a ranking and feasibility tool, not a camera model.

`python3 contrib/af_novel/focusfeed_probe.py` reproduces the `(9,0x17,32)` observations in
N1 and prints the decoding protocol.

---

## 6. What not to pursue

- **Absolute defocus from a single image without a scene reference** (single-frame band, or
  `s/g` thresholds): measured cross-scene spread is as large as the signal. Use the
  differential form.
- **A 3-point parabola/Gaussian vertex as a jump** (N4): offline it is worse than the
  existing retrace at real noise; the second difference amplifies noise.
- **Stop-on-threshold during a `(9,1)` move**: `(9,3,1)` does not halt the move; dead.
- **Edge width as a fine-focus metric** (N3): pixel pitch saturation.
- **Waiting for `0x8005001E` / `(9,0x14)` focus-pulse property**: it is MPU→ICU only and
  never arrives on the EOS M; use N15a (monitor read) instead.
- **Chromatic-aberration / per-channel defocus** in real time: raw channels are not in the
  LV buffer.
- **A CNN / learned policy on the camera**: the EOS M ARM budget and the lack of a large
  aligned dataset make this strictly worse than N2 for the same session cost.

---

## 7. Open questions that one field session should answer

1. Does `(9,0x17,32)` encode defocus (N1), and if so is it signed or magnitude only?
2. Does `MpuMonRead` expose a motor-step counter during `(9,1)` moves (N15a)?
3. What is the pulse stroke between end stops, and how many pulses per DOF at 18/35/50 mm
   (N12/N13)?
4. Does `focus_dist`/`LV_LENS` track Canon's own AF accurately enough to anchor the scale
   (N6)?
5. Can the preset be dropped to x1 for a Canon half-press and restored cleanly, including
   immediately before recording (N10a)?
6. Is x10 measurable with the preset active (N9)?
