# EOS M MPU autofocus — faster focus algorithms

**Date:** 2026-10-19
**Camera:** Canon EOS M, firmware 2.0.2, EF-M 18-55 STM
**Context:** the MPU AF search is now reachable from ML; the current
sharpness-peak-stop implementation is in `crop_rec`. A first camera run was
made on 2026-10-19, but its diagnostic record was lost; the build now writes a
per-sample `AFSEARCH.LOG` trace unconditionally and arms the peak stop only on
the far leg. **The first useful trace is still pending**, so all threshold
tuning below should start from that log.
**Question:** given the focus commands we now have, which modern algorithms
could make autofocus faster and more reliable?

This note is a research survey plus a concrete implementation plan. It assumes
the command set and limitations documented in
[`EOSM-MPU.md`](EOSM-MPU.md) and
[`EOSM-MPU-HANDOVER.md`](EOSM-MPU-HANDOVER.md); it does not repeat the MPU
protocol.

**Update:** a first `AF drive (MPU)` implementation using the raw `(9,1,5)`
relative move is now in the repo (`afdiag_afdrive_task()` in
`crop_rec.c`). It does a direction probe, coarse climb and fine climb. It is
not yet field-validated, but if it works the "needs a bounded/relative drive"
algorithms below become reachable. The implementation-level review and v1
proposal are in [`EOSM-AF-IMPLEMENTATION.md`](EOSM-AF-IMPLEMENTATION.md) §4.2.

---

## 1. Bottom line

1. **The current bottleneck is the actuator, not the algorithm.** The confirmed
   `(8,2,32)` command was observed to drive to the near end first and then sweep
   to infinity; it takes about 8.8 s. No search algorithm can skip that near
   leg. The fastest realistic gains are:
   - **now:** predictive braking + a better peak detector + a better focus
     measure (a few hundred milliseconds, fewer failed runs);
   - **after a bounded/relative drive works:** one-dimensional search
     (Brent/golden-section/parabolic fit) in roughly 4–10 lens moves;
   - **best case:** a differential-depth-from-defocus (DFD) jump that reaches
     the focus plane in 1–2 moves, then one refinement.
2. **If the job is video / Auto refocus, a full sweep is not viable.** Even a
   perfect stop still spends seconds going to the near end. Continuous AF needs
   a relative or direction-controlled drive, plus tracking.
3. **"Modern" does not mean "deep learning on the camera" here.** The most
   useful modern ideas are model-based: Gaussian defocus models, Bayesian
   optimization for noisy 1-D search, differential DFD, and learned focus
   position as a longer-term option. The EOS M does have Canon's Hybrid CMOS AF
   (phase-difference pixels used together with contrast AF), but ML has no
   documented access to the raw phase signal — the MPU consumes it internally.
   So a PDAF-style one-step measurement is not available as a drop-in; the
   practical target is a smarter contrast/sharpness search plus a bounded
   actuator. Large CNN approaches also do not fit the camera.

A good rule for this project: **minimise lens travel and braking distance, not
the number of image samples.** A 50 ms sample is cheap; 100 motor steps are
not.

---

## 2. What the current commands can and cannot do

| Message | Meaning | Status | Relevance for search |
|---|---|---|---|
| `(8,2,32)`, payload dword 4..7 = `0x10` | `AF_LENS_SEARCH` `0x33794` | confirmed on camera | starts a full **near → infinity** sweep; stop only |
| `(9,3,1)`, `[00]` | focus stop | confirmed | ends the sweep; `FOCUS_DONE` follows |
| `(9,0x13,4)`, `{gate,b1,b2,b3}` | `LensSearchDriveStart` | reverse-engineered, **not camera-tested** | likely `{end, speed, timeout}`: direction + speed + bounded drive |
| `(9,1,5)`, `{hi,lo,0x07,0,0}` | relative focus pulses | raw form **not camera-tested**; Canon property path was a no-op | the ideal primitive for hill-climbing and curve fitting |
| `(9,0x25,16)` | AF session | Canon sends this on half-press | may be required for the two commands above |
| `(9,0x0B,2)` | focus state | known | Canon sends `00` when AF ends |
| `0x80050000` `LV_LENS` | 61-byte lens snapshot | confirmed, ~3 Hz | `focus_pos` at `0x22` (not reliable here), `focus_dist` at `0x30/0x32` |
| `0x8005001E` pulse count | motor position | never observed on EOS M | would give a real position if it ever arrives |

Feedback available today:

- centre Laplacian sharpness, sampled every 50 ms from the displayed LV buffer
  (cheap, but flat when focus is far off);
- `lens_info.focus_dist`, coarse centimetres, `65535` = infinity, jittery;
- `LV_LENS` focus phase/position fields, about 3 updates per second;
- no reliable `focus_pos` on this camera.

This asymmetry explains the algorithm choices below: **we can observe the
image continuously, but we cannot move the lens to an arbitrary position yet.**

---

## 3. Cost model: where the time actually goes

For a bounded move, a useful first-order model is

```
T_total ≈ T_command + T_travel(|Δp| / v) + n_settle * T_settle + T_brake
```

where `T_command` is the MPU round trip (tens of ms), `v` is the focus-motor
speed, `n_settle` is the number of moves, and `T_brake` is the command latency
plus lens deceleration. Sampling and image processing at 20–30 Hz are small
terms.

Consequences:

- **Golden-section / Bayesian optimisation reduce the number of samples but not
  necessarily the total travel.** Golden-section on a full range can move the
  lens almost as far as a sweep. It wins when the search range is already
  narrow, or when starting from the current focus.
- **Curve fitting and DFD win because they replace a scan with a computed
  jump.** That is the direction the recent literature is moving.
- **Predictive braking wins because it removes the detection delay after the
  peak.** The current 80 %/120 ms rule necessarily stops after the peak, then
  the lens coasts further. A model can fire the stop earlier.

Known field numbers: full sweep ≈ 8.8 s, 27–29 `LV_LENS` updates. The
peak-stop trace is still pending; until it exists, the stop thresholds can only
be reasoned about, not measured. The peak-stop variant still has to cover the
near leg before it can arm on the far leg.

---

## 4. Algorithms that work with the current full-sweep command

These are safe to try without any new MPU message. They cannot remove the near
leg, but they can stop closer to the true peak, reduce overshoot, and avoid
wasted retries.

### 4.1 Predictive braking / model-based peak stop

The current rule is a trailing detector:

```
stop when S < 0.80 * peak for > 120 ms
```

By construction it stops *after* the peak, and the lens keeps moving while the
command is in flight. Replace it with a short-horizon predictor:

```
model near the peak:   S(t) = B + A * exp(-(t - t*)^2 / (2*tau^2))
```

Equivalently, fit a parabola to the last 5–7 samples of `S` (or of
`log(S - B)`, or of the scene-normalised `q = S/G`) and use its vertex:

```
S(t) ≈ a*t^2 + b*t + c,   a < 0
t*   = -b / (2a)
S*   = c - b^2 / (4a)
```

Send `(9,3,1)` when `t* - t_now <= T_brake` and the fitted peak is prominent
enough (for example `S* > 1.3 * S_at_arm`). `T_brake` is calibrated once from
field logs: stop at fixed delays on both sides of the peak, measure the
settled sharpness, and fit the relation between stop time and settled peak.
Update it per lens if the logs show different coast times.

Practical details:

- fit only on the armed far leg, never on the near leg;
- reject fits with `a >= 0` or with a vertex outside the plausible horizon;
- use a robust fit (Huber / Theil–Sen on 5 points) so one exposure step cannot
  move the vertex;
- keep the old 80 %/120 ms rule as a fallback if the fit is unstable;
- after the stop, keep sampling for 300–500 ms and log `settled_peak`,
  `overshoot = (S_at_stop - S_settled) / S_peak`, and the final distance.

Expected gain: stop 200–500 ms earlier and reduce overshoot. This is a modest
win against an 8.8 s sweep, but it is cheap, safe, and improves every later
experiment.

### 4.2 Sequential peak detection instead of a fixed drop

The fixed 80 % threshold and 120 ms dwell are hand-tuned to one scene and one
noise level. Replace them with a statistical detector:

- estimate the noise standard deviation `sigma` from the high-frequency
  residual of the sharpness trace;
- run a **CUSUM** on the residual from the fitted peak:
  `g_k = max(0, g_{k-1} + (peak - S_k)/sigma - nu)`, stop when `g_k > h`;
- or fit a local slope over the last 4–5 samples and stop when the posterior
  probability of a negative slope exceeds 0.95;
- or use Bayesian online changepoint detection (BOCPD) on the sign of the
  slope.

This adapts the dwell to the noise: in good light it stops sooner; in low light
it does not false-trigger on a noisy sample. It also gives a principled
`stopped`/`timeout` decision for the log.

### 4.3 A better focus measure (cheap, high leverage)

The current metric — Laplacian sum over the centre quarter — is good at telling
"sharp" from "blurry" but poor for search:

- it is flat when focus is far off, so it gives no gradient for hill-climbing;
- it is scene-dependent and exposure-sensitive;
- a single hot pixel or noisy frame can create a false peak.

A better search metric combines three cheap ideas:

1. **Multi-scale band energy.** Compute 3 band-pass energies (for example
   differences of Gaussians at σ = 1, 2, 4 pixels). Deep defocus kills the high
   band but leaves the low band; near focus the high band dominates. Use the
   ratio `E_high / (E_mid + E_low + eps)`, or select the band with the largest
   current dynamic range. This extends the monotonic range of the metric,
   which is exactly what the current metric lacks.
2. **Scene normalisation.** Divide by local gradient energy or local contrast,
   as Auto refocus already does with `q = S/G`. For peak search, keep the raw
   `S` for the peak value and use the normalised `q` for decisions.
3. **Temporal filtering.** Use the median of 3 frames, or a small Kalman
   filter, before fitting the peak. This costs 1–2 frames but prevents a
   one-frame spike from triggering the stop.

A small learned focus measure is also possible: the 2015 Pattern Recognition
Letters work showed supervised learning improves contrast AF accuracy and
low-light performance. On the EOS M the practical version is a linear
combination of a few filter energies trained offline on logged LV frames, not
a CNN.

### 4.4 Coarse-to-fine sampling (limited without direction control)

During a full sweep, sampling faster far from the predicted peak and denser
near it saves CPU but not motor travel. It becomes genuinely useful only when
the lens can be stopped and moved back, because then the coarse stage can
bracket the peak and the fine stage can spend moves only inside the bracket.
Keep this idea for Section 5.

---

## 5. Algorithms that need a bounded or relative drive

Everything below assumes that at least one of these becomes usable:

- `(9,0x13,4)` with a direction/end selector and a speed, giving a bounded
  search drive;
- `(9,1,5)` relative pulses, giving direct moves of a computed size.

**Update:** the repo now has an `AF drive (MPU)` implementation that
sends raw `(9,1,5)` `[hi lo 07 00 00]` moves and runs a direction probe plus a
coarse/fine hill climb. It is not yet field-validated, but the algorithms in
this section are now implementable, not just paper designs. See
[`EOSM-AF-IMPLEMENTATION.md`](EOSM-AF-IMPLEMENTATION.md) §4.2 for the review and
the v1 proposal. `(9,0x13)` remains an optional coarse-move optimisation.

### 5.1 Parabolic / Gaussian curve fit and secant iteration

This is the classic "curve-fitting AF" family and the one to implement first.
It needs only relative moves and a sharpness value per position.

Near the focus plane the sharpness profile is approximately Gaussian:

```
S(p) = B + A * exp(-(p - p*)^2 / (2*sigma^2))
```

Procedure:

1. take 3 samples bracketing the peak, spaced by roughly the depth of field;
2. fit the Gaussian (or fit a parabola to `log(S - B)`; with the baseline `B`
   fixed or approximated from the tails, 3 points determine `A`, `p*` and
   `sigma`);
3. move to the fitted vertex `p*`;
4. take one verification sample, refit with the bracket, move once more;
5. stop when the predicted movement is below the depth-of-field step.

With a good initial bracket this converges in **4–6 evaluations**. It is the
lowest-latency option short of DFD, and it is robust to the peak being broad.
A secant/Newton variant on `dS/dp` is equivalent but more sensitive to noise;
prefer the 3-point parabolic fit.

The 2024 lens-positioning paper by Karaoglu gives the optical formulas for
choosing those sample positions from focal length, aperture, circle of
confusion and depth of field, so the step sizes can be derived instead of
hand-tuned. It cites the same classical family: Fibonacci, rule-based,
hill-climbing, and the curve-fitting methods ROL and BPIC.

### 5.2 Golden-section / Fibonacci / Brent

For a unimodal, noisy-free 1-D function, these are the optimal derivative-free
search methods.

- **Golden-section:** bracket `[a,b]`, evaluate at
  `c = b - 0.618(b-a)` and `d = a + 0.618(b-a)`, keep the better sub-bracket,
  repeat. About `log((b-a)/tol) / log(1.618)` evaluations — roughly 10–12 for
  a 100:1 range and a 5-step tolerance.
- **Fibonacci search:** the fixed-budget version of the same idea; useful when
  the allowed number of frames is known in advance.
- **Brent's method:** golden-section plus parabolic interpolation, with a
  fallback when the parabola is unreliable. In practice it is the best
  general-purpose choice: superlinear near the peak, still robust far away.

Golden-section and Brent are the right fallback when the sharpness curve is
unimodal but the Gaussian fit fails. Their weakness here is total travel: from
a cold start they can traverse most of the range. Seed the bracket from the
current focus or from the last successful focus to keep travel small.

For a focus stack rather than a single AF, the same math gives efficient
non-uniform bracketing: dense samples near the peak, sparse samples in the
tails, instead of a uniform sweep.

### 5.3 Bayesian optimisation / Gaussian processes / BPIC

If the sharpness signal is noisy (low light, moving subject, exposure
flicker) or the curve has several local maxima, replace the deterministic
search with a sample-efficient global optimiser:

- put a Gaussian-process prior on `f(p) = log S(p)` (Matérn 5/2 is a good
  default);
- after each observation, compute the posterior mean and variance;
- choose the next position by expected improvement (EI) or UCB;
- stop when the predicted improvement is below the depth-of-field tolerance,
  or when the budget is exhausted.

In 1-D this typically needs 8–12 evaluations, more than a good parabolic fit,
but it is much more robust to noise and local peaks and it returns an
uncertainty estimate that can gate the "AF OK" decision. A GP can also be
constrained with the physical prior "increasing on the near side, decreasing on
the far side", which removes most of the multi-peak ambiguity.

The 2024 IEEE work "Deep Reinforcement Learning-Based Camera Autofocus with
Gaussian Process Regression" uses a GP to model the focus response and a
learned policy to choose moves; the GP part is the transferable idea. The
older BPIC method cited by the lens-positioning paper is an AF-specific
Bayesian peak search in the same spirit.

Recommendation: keep BO/GP as the **noisy-scene fallback**, not the default.
For a clean static scene Brent or the parabolic fit will be faster.

### 5.4 Differential depth-from-defocus (DFD): the one-step jump

This is the most promising "modern" route to sub-second AF on this hardware.

Classical DFD uses the fact that defocus is a low-pass filter. If two images
are taken at two known focus positions `p1` and `p2`, the *relative* blur
between them cancels the unknown scene texture:

```
|F(I2)|^2 / |F(I1)|^2 ≈ exp(-k * omega^2 * (sigma2^2 - sigma1^2))
```

(`k` depends on the Fourier/PSF convention; only the slope against `omega^2`
matters.)

Fit the slope of the log spectral ratio against `omega^2` over a mid-frequency
band to estimate the relative blur `|sigma2^2 - sigma1^2|`; the sharper frame
gives the sign. A calibration curve `sigma(p)` (per lens, focal length and
aperture) then gives the distance from the current position to the focus
plane, and therefore the relative move to make.

Procedure:

1. capture frame A at the current focus;
2. move by a known small step (relative drive);
3. capture frame B;
4. estimate relative blur and invert the calibration to get `delta_p`;
5. move to `p* = p_current ± delta_p`;
6. one parabolic refinement step.

Expected cost: 2 frames plus 2–3 moves, versus 5–10 moves for a search. The
recent DFD literature is active — Gaussian defocus models were re-validated
for ordinary imaging devices in 2026, and direct-optimisation and zero-shot
learned DFD methods are appearing — but the classical two-frame version is the
one that fits an ARM camera with no GPU.

Caveats: it needs texture in the ROI, a calibrated blur curve, and stable
exposure. Use the same centre ROI and mid-frequency band as the focus measure.
A cheap approximation is to use the multi-scale band energies from Section 4.3
instead of a full FFT: the ratio of high-to-low band energy is a scalar
relative-blur estimate.

Implementation caveat: two frames give a **relative** blur, not an absolute
distance to focus. Turning it into a jump needs a calibrated `blur ↔ focus
error` curve plus a prior or a third sample to resolve the side ambiguity. Treat
DFD as a calibrated coarse estimator rather than a guaranteed one-step solver;
[`EOSM-AF-IMPLEMENTATION.md`](EOSM-AF-IMPLEMENTATION.md) §3.7 has the concrete
procedure.

Note that on the current full-sweep command the same idea can be used
*open-loop*: while the lens is sweeping, two frames separated by a known time
give the remaining defocus, and the known sweep speed gives the time at which
the focus plane will be reached. That turns peak detection into a prediction
and can fire the stop before the peak. It is a good experiment even before a
relative drive exists, but it inherits the near-leg problem.

### 5.5 Learned focus-position prediction (longer term)

The 2020 "Intelligent Autofocus" work showed that a CNN can predict the best
focus position from **1–2 image samples**, reporting 5–10× faster focus than
search-based methods and no special hardware. The 2025 CVPR event-camera work
pushes the same idea to a one-step focus *detection* function (Event Laplacian
Product), cutting focus time by up to two-thirds versus a focus stack.

For the EOS M this is attractive but not immediate:

- training data can be collected from the AFSEARCH logs plus saved LV frames;
- a full CNN is too heavy for the camera, but a tiny model or a linear
  regressor over multi-scale band energies, `focus_dist`, zoom and aperture is
  feasible;
- the model only predicts a jump; the actual move still needs a relative
  drive;
- the DFD route is easier to calibrate and debug than a learned model.

Treat this as Phase 4, after the classical curve fit and DFD are working. The
event-camera paper is useful mainly as a design pattern: **predict the focus
error from one or two observations instead of scanning the whole range.**

One reverse-engineering question is worth checking before any training: whether
the per-frame AF data feed `(9,0x17,33)` or the AF configuration block
`(9,0x21,61)` contains a phase/disparity/defocus number that Canon already
computes. The EOS M has Hybrid CMOS AF hardware, so the MPU may have a
one-step phase measurement that never reaches ML. If such a field exists, the
one-step jump becomes a decoding problem instead of a learned model. This is
speculative; no field has been identified yet.

---

## 6. Continuous AF and tracking

For video, the number of searches matters more than the speed of one search.
A full sweep every time the subject moves is unusable. Once bounded moves work:

- keep a Kalman filter on `[p, v]` (position and velocity of the focus plane),
  with the curve-fit vertex as the measurement;
- predict where the focus plane will be in the next few frames;
- only move when the predicted blur exceeds a deadband, and prefer small
  corrective moves over a new search;
- use a particle filter only if the scene has several depth layers and the
  centre ROI is ambiguous.

This is the same "focus prediction and tracking" idea used by phase-detection
and dual-pixel cameras, but implemented with contrast measurements. It does
not replace the need for a bounded drive.

Model-predictive control of the lens trajectory (for example LSTM-based MPC for
piezoelectric AF stages) is a mature idea in the control literature. It does
not apply directly here because the EOS M MPU does not expose a trajectory
interface: our only levers are "stop" and, if it works, a relative step. The
predictive-braking idea in Section 4.1 is the simplified version of the same
control problem.

---

## 7. Recommended implementation plan

Implementation-level details — component code, reusable open-source
references, and the combined cascade — are in
[`EOSM-AF-IMPLEMENTATION.md`](EOSM-AF-IMPLEMENTATION.md).

Order-of-magnitude comparison (to be replaced by measured numbers from Phase 0;
the motor speed dominates, so treat the times as targets, not predictions):

| Approach | Needs | Typical budget | Main risk |
|---|---|---|---|
| Predictive braking + statistical peak stop | current command | 0 extra moves; saves 0.2–0.5 s | fit instability near the peak |
| Multi-scale normalised metric | current command | 0 extra moves | extra CPU; needs calibration |
| Parabolic / Gaussian fit | relative drive | 4–6 evaluations, ~1–2 s | needs a good initial bracket |
| Brent / golden-section | relative drive | 6–10 evaluations, ~1.5–2.5 s | total travel can be large |
| GP / Bayesian optimisation | relative drive, noisy scene | 8–12 evaluations, ~2–3 s | slower on clean scenes |
| Differential DFD jump | relative drive + blur calibration | 2–3 moves, < 1 s | calibration and texture dependence |
| Learned focus position | relative drive + training data | 1–2 frames, < 0.5 s | data, model size, validation |

Detailing the phases:

### Phase 0 — offline replay and instrumentation (no camera risk)

1. Extend `AFSEARCH.LOG` with the data needed to compare algorithms offline:
   `t`, raw `S`, `G`, `luma`, `emax`, per-band energies, `focus_dist`,
   `LV_LENS` phase, and the predicted `t*`/`S*`.
2. Write a small host-side replay script that parses the log and re-runs the
   stop decision. This lets us compare 10 algorithms on one field run without
   flashing the camera 10 times.
3. Characterise the metric: sharpness vs `focus_dist` at several distances,
   textures, zoom levels and apertures. This is the single most useful dataset
   for every later choice.

### Phase 1 — better stop on the current command (camera build)

Implement, behind the existing diagnostic action:

- scene-normalised, multi-scale focus measure (Section 4.3);
- predictive braking with a fitted `t*` and a `T_brake` from the logs
  (Section 4.1);
- CUSUM/BOCPD peak detection as the fallback (Section 4.2);
- post-stop overshoot logging.

Acceptance: equal or better final sharpness, stop 200–500 ms earlier, overshoot
below roughly 10 %, no increase in timeouts or false stops across the test
matrix.

### Phase 2 — characterise the bounded commands

The committed `AF drive (MPU)` already exercises `(9,1,5)`; validate it
first, then characterise the alternatives. Test, one command per run, with the
existing safety rules and a stop after every drive:

- `(9,1,5)` relative pulses in both directions, with the `0x07` speed byte,
  using `mpu_lab` Drive test and `AFDRIVE.LOG`;
- `(9,0x13,4)` with `{end, speed, timeout}` variants, after `(9,0x25,16)`;
- whether `0x8005001E` (motor position) or any `(9,0x14)` feedback arrives.

Measure: direction, speed (pulses/s), latency, repeatability, minimum reliable
step, whether two moves accumulate, and **pulses per depth of field**. This
sets the coarse/fine step sizes and the Brent tolerance.

### Phase 3 — closed-loop search

Now that a relative drive exists:

- parabolic/Gaussian fit first (4–6 evaluations);
- Brent as the robust fallback;
- golden-section only for a cold, wide, clean search.

Acceptance: focus in 5–8 moves and under about 1.5 s, repeatable within the
depth of field on the test matrix.

### Phase 4 — DFD and tracking

- calibrate `sigma(p)` per lens/aperture with a textured chart;
- implement the two-frame relative-blur estimate and the one-step jump;
- add the Kalman tracking loop for video;
- only then consider the learned focus-position model.

---

## 8. What not to do

- **Do not optimise frame rate first.** The motor travel dominates; a 2× faster
  sharpness sample does not shorten an 8.8 s sweep.
- **Do not hill-climb from deep blur with the current Laplacian.** It is flat
  there; the gradient is noise.
- **Do not use a full sweep for video or Auto refocus.** It is a scanner, not
  a focus actuator. Keep Canon's half-press until a bounded drive works.
- **Do not start the sweep at infinity.** The freeze is real; keep the guard.
- **Do not ship a CNN or a DRL policy to the camera** before there is a replay
  dataset and a bounded drive. The literature methods assume a controllable
  lens and a training set that we do not have yet.
- **Do not tune thresholds on one scene.** Log distance, texture, light, zoom
  and lens; evaluate across them.
- **Do not change several algorithm parts in one field run.** The logs are the
  only ground truth; make each run answer one question.

---

## 9. Sources and further reading

Command set, current implementation and constraints:

- [`EOSM-MPU.md`](EOSM-MPU.md) — protocol, `(8,2,32)`, `(9,0x13)`, `(9,1,5)`,
  `LV_LENS` feedback, field runs.
- [`EOSM-MPU-HANDOVER.md`](EOSM-MPU-HANDOVER.md) — current status and next
  steps.
- [`EOSM-AF.md`](EOSM-AF.md) — earlier lens-drive attempts, sharpness metric,
  Auto refocus.

Algorithms and recent work:

- A. Karaoglu, *Optimal Sample Lens Positioning in Digital Camera Systems*,
  2024 — optical formulas for sample positions; good overview of Fibonacci,
  hill-climbing and curve-fitting AF.
  <https://arxiv.org/abs/2407.01789>
- C. Wang, Q. Huang, M. Cheng, Z. Ma, D. J. Brady, *Intelligent Autofocus*,
  2020 — learned focus position from 1–2 images, 5–10× faster than search.
  <https://arxiv.org/abs/2002.12389>
- Y. Bao, S. Gao, W. Li, K. Wang, *One-Step Event-Driven High-Speed Autofocus*,
  CVPR 2025 — focus as detection, not search; event sensor required.
  <https://arxiv.org/abs/2503.01214>
- *Deep Reinforcement Learning-Based Camera Autofocus with Gaussian Process
  Regression*, IEEE 2024 — GP focus model plus learned move policy.
  <https://ieeexplore.ieee.org/document/10849790>
- A. Saadat, *Defocus Aberration Theory Confirms Gaussian Model in Most Imaging
  Devices*, 2026 — justifies the Gaussian blur model used by DFD and by the
  curve fit.
  <https://arxiv.org/abs/2601.04779>
- H. Jackson, C. Adams, I. Lopez-Francos, B. Recht, *Depth from Defocus via
  Direct Optimization*, 2026 — modern global DFD; useful as a reference for
  the forward model, too heavy to run on the camera.
  <https://arxiv.org/abs/2602.18509>
- Y. Zuo et al., *Zero-Shot Depth from Defocus*, ECCV 2026 — learned DFD from a
  focus stack with zero-shot generalization; long-term reference, not a
  two-frame method.
  <https://arxiv.org/abs/2603.26658>
- *LSTM-Based Model Predictive Control of Piezoelectric Motion Stages for
  High-Speed Autofocus*, IEEE Transactions on Industrial Electronics, 2022 —
  trajectory control; relevant only if a trajectory interface ever appears.
  <https://ieeexplore.ieee.org/document/9846866>
- *Improving the accuracy and low-light performance of contrast-based autofocus
  using supervised machine learning*, Pattern Recognition Letters, 2015 —
  learned focus measures for contrast AF.
  <https://doi.org/10.1016/j.patrec.2015.01.010>

Classical background (no link needed for the well-known methods):

- Kiefer's Fibonacci search and the golden-section method for unimodal
  1-D optimisation;
- Brent's method for derivative-free minimisation;
- Tenengrad / Laplacian / mid-frequency focus measures;
- CUSUM and Bayesian online changepoint detection.
