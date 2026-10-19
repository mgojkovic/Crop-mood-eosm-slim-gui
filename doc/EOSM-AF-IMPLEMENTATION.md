# EOS M MPU autofocus — implementation research

**Date:** 2026-10-19
**Camera:** Canon EOS M, firmware 2.0.2, EF-M 18-55 STM
**Companion to:** [`EOSM-AF-ALGORITHMS.md`](EOSM-AF-ALGORITHMS.md) — that note
compares algorithm families; this one is about **how to build them** in
`crop_rec`, what to reuse, and how to combine several of them into one
controller.

The short version: the realistic implementation is a **cascade**, not one
clever search. Use cheap global acquisition, a model-based jump, a local
refinement, and a confidence gate; fall back to the full sweep or Canon AF when
a stage fails. Most of the pieces already exist in the repo or in mature
open-source camera-autofocus code.

Status: the repo has the first full-sweep stop implementation and an always-on
`AFSEARCH.LOG`, but the first useful field trace is still pending. In addition,
the repo now contains **`AF drive (MPU)`** (commit `7be6799`): a one-shot contrast AF using
raw `(9,1,5)` relative moves, a direction probe, a coarse climb and a fine
climb. It is the first Pipeline B (Section 4.2) implementation and it is **not
yet field-validated**. This note reviews that loop and proposes a v1 that keeps
its command layer but replaces the fixed-step climb with a model-based fit.
Phase 0 (capture and replay) still comes before threshold tuning.

---

## 1. What the build gives us

The module environment is more constrained than a desktop, but enough for all
of the useful algorithms:

| Constraint | Value | Consequence |
|---|---|---|
| Compiler | `arm-none-eabi-gcc`, `-Os`, `-march=armv5te` | no NEON; keep loops small |
| Floating point | soft float, `-ffast-math`, `-fsingle-precision-constant` | use `float`, avoid `double` and heavy `expf`/`logf` in hot loops |
| Math library | `newlib-libm.a` is linked; `sqrtf`, `expf`, `logf` already used in `src/` | one-off fits are fine |
| Memory | static buffers preferred; `malloc` exists but the module heap is shared | a 128-sample ring and a few 8 KB buffers are safe |
| Tasks | `task_create`, current AF task stack `0x2000` | raise to `0x3000` if the fit code grows |
| Timing | `get_ms_clock`, `msleep`; LV sharpness at 50 ms | control loop is 20 Hz, lens is the slow part |
| Command path | `MPU_SEND` at RAM `0x38C4`; Canon property calls also available | raw `(9,1,5)`, `(9,0x13,4)`, `(8,2,32)` are all sendable |

Existing pieces to build on:

| Component | Location | What it already does |
|---|---|---|
| `afdiag_sharpness_ex()` | `modules/crop_rec/crop_rec.c` | centre-quarter Laplacian + gradient + luma + max edge |
| `afdiag_afsearch_task()` | same file | full sweep, far-leg arming, log, post-stop sampling |
| `AFSEARCH.LOG` writer | same file | per-sample trace: `ms s g luma dist phase` |
| `afdiag_afdrive_task()` / `afd_run()` | same file (commit `7be6799`) | raw `(9,1,5)` relative drive, direction probe, coarse/fine climb, `AFDRIVE.LOG` |
| `afd_move()` / `afd_sample()` | same file (commit `7be6799`) | one relative move with `FOCUS_DONE` wait, settle and 2-frame sharpness sample |
| `afdiag_mpu_build()` / `afdiag_mpu_canon_request()` | same file | property-path MPU requests, including `(9,0x13,4)` |
| `MPU_SEND` and `cap_send()` | `modules/mpu_lab/mpu_lab.c` | raw group/op/len send; already used for `(9,0x13)` and `(9,1,5)` |
| `lens_focus()` | `src/lens.c` | `PROP_LV_LENS_DRIVE_REMOTE`; **clamps step size to ±3**, so not enough for coarse moves |
| `aref_*` Auto refocus | `modules/crop_rec/crop_rec.c` | sharpness monitor + Canon half-press request; must be blocked during a search |
| `focus_stack()` | `src/focus.c` | step/settle patterns and command pacing, not a search |

The important gap: **there is no command abstraction yet**. The task currently
calls the raw `0x38C4` pointer directly and uses `lens_info.focus_dist` as its
only position sense. The first implementation step is to put a small actuator
layer around those calls.

Current action limits to keep in mind:

- `AFS_MAX_MS` is 3000 ms, but the full sweep to infinity took about 8.8 s.
  For a far subject the peak can easily be after 3 s, so the current action may
  time out before it arms or reaches the far-leg peak. The first trace will
  show the real time-to-peak; do not raise the cap blindly.
- The log records one sharpness value. The multi-scale metrics and the fit
  inputs do not exist yet.
- `afdiag_afsearch_busy` and `afdiag_afdrive_busy` are not part of the Auto
  refocus gate, so a background Auto refocus request could overlap a search.
- The sample loop reads `focus_dist` at 20 Hz even though the property updates
  at about 3 Hz; the arming logic must not treat the repeated value as new
  information.
- The new `AF drive (MPU)` loop uses fixed steps (80 coarse / 20 fine pulses)
  and a 1500-pulse travel cap. Those numbers are not calibrated to the lens or
  the depth of field yet, and `AF drive` has no infinity/end-stop guard of its
  own.
- `AF drive` waits `AFD_SETTLE_MS` + two frames per move, plus a minimum
  `100 ms + 2 ms/pulse` (capped at 700 ms). An 80-pulse step is therefore
  roughly 350–450 ms, and 25 coarse moves can take 8–10 s. The fit-based
  replacement should cut both the move count and the per-move wait.

These are all fixable in Phase 1/2 and most do not require new MPU commands.

Current status at a glance:

| Pipeline | State |
|---|---|
| A: full sweep + peak stop | implemented; first useful trace pending |
| B: `AF drive (MPU)` v0 | implemented (commit `7be6799`); field validation pending |
| B: fit + Brent v1 | proposed in §4.2 |
| C: Kalman tracking | proposed in §4.3 |
| DFD coarse estimate | proposed in §3.7; needs calibration |

---

## 2. Reference implementations worth studying

These are the concrete codebases to borrow *ideas* from. Licenses matter: ML is
GPLv2, so BSD/MIT code can be ported with attribution; LGPL code is best used
as a reference or linked carefully; Apache-2.0 code should not be copied into
GPLv2 sources.

### 2.1 libcamera common contrast AF (`AfHillClimbing`)

The 2023 libcamera patch adds a platform-independent contrast AF state machine:

- coarse scan with a large step, then fine scan in a range around the coarse
  result;
- the derivative test is `current - max >= -(max * 0.1)`, i.e. a 10 % tolerance
  before declaring the peak passed;
- `setFramesToSkip()` handles the lens/statistics delay;
- continuous mode compares the current contrast against the remembered maximum
  and re-triggers when the variance change ratio exceeds a threshold.

This is a clean model for a `AF_PHASE_COARSE` / `AF_PHASE_FINE` state machine
and for the frame-skip problem that the EOS M also has.

- Patch and full source:
  <https://lists.libcamera.org/pipermail/libcamera-devel/2023-March/037336.html>
- Older Raspberry Pi `focus.cpp` (different algorithm, same control shape):
  <https://chromium.googlesource.com/chromiumos/third_party/libcamera/+/fd2b55cb3f2b50e273e01f422c251c18ffa441ef/src/ipa/raspberrypi/controller/rpi/focus.cpp>

### 2.2 Micro-Manager `OughtaFocus` — Brent + metric menu

`OughtaFocus` is the most directly reusable reference:

- wraps the focus axis in a `UnivariateRealFunction`;
- calls Apache Commons Math `BrentOptimizer` with `GoalType.MAXIMIZE`, a
  search range around the current position and an absolute tolerance;
- exposes ten scoring methods: `Edges`, `StdDev`, `Mean`,
  `NormalizedVariance`, `SharpEdges`, `Redondo`, `Volath`, `Volath5`,
  `MedianEdges`, `FFTBandpass`;
- crops the ROI and sets exposure before the search, then restores both.

The Apache-2.0 optimizer source should not be copied into ML, but the *design*
is exactly the "Brent over a focus score" pattern. Brent itself is easy to
implement clean-room from the classic algorithm; Burkardt's C version is a
useful reference (LGPL):

- <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/autofocus/OughtaFocus.java>
- <https://people.math.sc.edu/burkardt/c_src/brent/brent.html>

### 2.3 Micro-Manager `Autofocus.java` — coarse/fine two-stage search

A simpler exhaustive two-stage search: coarse step 2 with ±1 point, then fine
step 0.2 with ±5 points, stopping when the score drops by more than a fraction
of the best. It is a good baseline and a reminder that a **two-stage bracket**
is often enough; the model-based fit is what removes the back-and-forth.

- <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/autofocus/Autofocus.java>

### 2.4 Micro-Manager `SimpleAutofocus/score.cpp` — metric implementation

A compact C++ focus score: normalised variance, 3x3 median filter, diagonal
edge kernel, Welford mean/variance. Useful as a clean reference for a robust
metric and for the "normalise by mean intensity" trick.

- <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/DeviceAdapters/SimpleAutofocus/score.cpp>

### 2.5 Enhanced mountain climbing (2025) — a real hybrid

This is the closest published "combo of many" to what we need:

1. focus measure = **Laplacian variance**, after threshold-based background
   denoising;
2. search direction from a **linear fit over the first five points**, not two;
3. mountain climbing with **fixed + dynamic slope thresholds**; step size is
   halved when the curve approaches a plateau, twice;
4. stop after three consecutive decreasing points;
5. **Legendre-polynomial quadratic least-squares fit** over the last ten points
   predicts the peak, so the stage does not have to move back and forth;
6. optional NIQE no-reference quality check and Richardson–Lucy restoration.

The paper reports large accuracy gains in fluorescence microscopy; the parts
that transfer to the EOS M are the five-point direction test, the dynamic slope
threshold, and the last-ten-point quadratic fit. NIQE is a host-side quality
check, not something to run on the camera. The code is a research Python
implementation with no explicit license, so treat it as a paper reference, not
a source to copy.

- Paper: <https://www.biorxiv.org/content/10.1101/2025.03.14.642633v1.full>
- Code: <https://github.com/jyt368/Autofocus>

### 2.6 DFD reference code

`hollyjackson/dfd` (MIT) implements joint depth/all-in-focus estimation from a
focal stack with a Gaussian forward model, grid search plus golden-section
refinement, and alternating minimization. It is far too heavy for the camera,
but its forward model and golden-section refinement are the right reference if
we ever build a DFD calibration on the host.

- <https://github.com/hollyjackson/dfd>
- `princeton-vl/FOSSA` (BSD-3) for the modern learned focus-stack variant:
  <https://github.com/princeton-vl/FOSSA>

### 2.7 Canon's mean-spectral-ratio DFD patent

Canon's US9836855B2 describes depth from the ratio of image spectra at
different focus settings. It is a useful explanation of why the relative-blur
estimate cancels texture, and of the calibration assumptions. It also makes the
ambiguity clear: a ratio gives a *relative* blur, not an absolute focus error,
without a model and a prior.

- <https://patents.google.com/patent/US9836855B2/en>

---

## 3. Component designs

### 3.1 Actuator abstraction

Put all lens traffic behind one small interface, so the search algorithms never
care whether the command is a raw MPU message or a Canon property:

```c
struct af_actuator
{
    int  (*move_rel)(int pulses, int speed);   /* (9,1,5), signed 16-bit   */
    int  (*search)(int end, int speed, int timeout); /* (9,0x13,4)        */
    int  (*stop)(void);                        /* (9,3,1)                  */
    int  (*get_pos)(void);                     /* pulse count, dist, ...   */
    int  (*settle)(int ms);                    /* wait / poll             */
};
```

Implementation notes:

- raw send: `((int (*)(int,int,int,const void *)) 0x38C4)(grp, op, len, data)`;
- relative move payload: `{ (pulses >> 8) & 0xFF, pulses & 0xFF, 0x07, 0, 0 }`;
  this is what `afd_move()` already sends in the current code;
- a relative move is finite: wait for `FOCUS_DONE` and a minimum time, then
  settle and sample (v0 does this); a **search** drive `(9,0x13,4)` has no
  natural end and must always be stopped with `(9,3,1)`;
- `(9,0x25,16)` session open/close around a sequence; `mpu_lab` already does
  this;
- `get_pos`: prefer `0x8005001E` (real pulse count) if it appears; otherwise
  use `lens_info.focus_dist` and a signed step accumulator;
- never start at `focus_dist >= 60000`; re-check after every menu close.

The wrapper must also expose a `busy` predicate so Auto refocus and the
existing `slim_af_state` machine cannot run at the same time. Right now
`afdiag_afsearch_busy` and `afdiag_afdrive_busy` only block another search /
drive; they are **not** checked by Auto refocus. Add one shared
`af_search_active` flag to the `aref` gate.

#### Combining the commands

The commands are complementary; a real search can use several in one session:

| Phase | Command | Role |
|---|---|---|
| session | `(9,0x25,16)` `[02,0…]` | put the MPU in the AF-session state; close with all-zero |
| global acquisition | `(8,2,32)` index 16 | confirmed full near→infinity sweep; fallback when nothing else works |
| coarse bounded move | `(9,0x13,4)` `{end,speed,timeout}` | direction/end + speed; needs characterisation |
| fine relative move | `(9,1,5)` `{hi,lo,0x07,0,0}` | exact pulse count for hill-climb / fit / Brent |
| stop | `(9,3,1)` `[00]` | always send after a drive; also ends the sweep |
| state | `(9,0x0B,2)` `[00,00]` | restore the idle state if a test changes it |

The intended hierarchy is: **session → coarse bounded move → fine relative
moves → stop → close session**, with the full sweep only as a fallback. The
the committed v0 is simpler: it uses only session + `(9,1,5)` relative moves. If
that works, `(9,0x13)` is an optimisation (fewer/larger coarse moves), not a
requirement. If `(9,1,5)` does not move the lens, `(9,0x13)` is the remaining
candidate for the coarse stage; if neither works, `(8,2,32)` is the only global
move and the controller is Pipeline A.

### 3.2 Sample ring and multi-metric measurement

Replace the single `s` value with a small record:

```c
struct af_sample
{
    uint32_t t_ms;
    uint32_t s_hi;      /* Laplacian, fine scale          */
    uint32_t s_mid;     /* Laplacian after 2x2 box blur   */
    uint32_t s_lo;      /* Laplacian after 4x4 box blur   */
    uint32_t g;         /* gradient energy                */
    uint32_t luma, emax;
    int16_t  dist;      /* lens_info.focus_dist           */
    int16_t  pos;       /* pulse count, or accumulated    */
    uint8_t  phase;     /* near / armed / post-stop       */
};
```

A single pass can compute all three Laplacian scales without a full image
buffer: the fine scale uses the existing every-2nd-pixel loop; the mid scale
uses a 2x2 box average at every 4th pixel; the low scale uses a 4x4 box average
at every 8th pixel. Each scale normalises by its sample count. Keep the ROI
identical to the current centre quarter so the new metrics are comparable with
the Auto refocus history. The useful derived values are:

```text
q     = s_hi * 100 / (g + 1)          /* sharpness per unit scene contrast */
m     = s_hi * 100 / (s_mid + s_lo + 1) /* high-band fraction               */
snr   = s_hi / noise_sigma
```

Why this matters: the current metric is flat far from focus, so the search has
no gradient there. The mid/low bands still respond when the high band is gone,
and `m` is a blur-scale indicator that stays monotonic further out. The exact
weights must be calibrated from logged LV frames; this is a Phase 0/1 task, not
something to guess in the field.

If the measurement cost becomes a problem, the cheap fallback is to compute the
mid/low bands only every third sample during the coarse phase and every sample
near the peak.

### 3.3 Quadratic least-squares fit

Use a centred coordinate to keep the normal equations well conditioned. With
`x_i = t_i - t_last` (or position minus current position), fit

```text
y = a*x^2 + b*x + c
```

by solving the 3x3 normal equations:

```text
[ Σx^4  Σx^3  Σx^2 ] [a]   [ Σx^2 y ]
[ Σx^3  Σx^2  Σx   ] [b] = [ Σx   y ]
[ Σx^2  Σx    n    ] [c]   [ Σy     ]
```

A compact C implementation:

```c
static float af_det3(const float m[3][3])
{
    return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
         - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
         + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
}

static int af_fit_quadratic(const float *x, const float *y, int n,
                            float *pa, float *pb, float *pc)
{
    float sx=0, sxx=0, sxxx=0, sxxxx=0, sy=0, sxy=0, sxxy=0;
    if (n < 3) return 0;
    for (int i = 0; i < n; i++)
    {
        float xi = x[i], yi = y[i];
        float x2 = xi * xi;
        sx += xi; sxx += x2; sxxx += x2 * xi; sxxxx += x2 * x2;
        sy += yi; sxy += xi * yi; sxxy += x2 * yi;
    }

    float m[3][3] =
    {
        { sxxxx, sxxx, sxx },
        { sxxx,  sxx,  sx  },
        { sxx,   sx,   (float)n },
    };
    float v[3] = { sxxy, sxy, sy };

    float det = af_det3(m);
    if (fabsf(det) < 1e-6f) return 0;

    float m0[3][3], m1[3][3], m2[3][3];
    memcpy(m0, m, sizeof(m)); memcpy(m1, m, sizeof(m)); memcpy(m2, m, sizeof(m));
    for (int r = 0; r < 3; r++)
    {
        m0[r][0] = v[r];
        m1[r][1] = v[r];
        m2[r][2] = v[r];
    }

    *pa = af_det3(m0) / det;
    *pb = af_det3(m1) / det;
    *pc = af_det3(m2) / det;
    return (*pa < 0.0f);   /* concave: a maximum exists */
}
```

The vertex is

```text
x* = -b / (2a)
y* = c - b*b / (4a)
```

Fit 5–7 samples, require `a < 0`, require the vertex to be within a sane
horizon, and check the residual. If the fit is unstable, fall back to the old
80 %/120 ms rule.

Numerical note: the snippet omits scaling for brevity. For a 5–7 sample window
spanning hundreds of milliseconds, divide `x` by `max|x|` and `y` by `max|y|`
before building the normal equations, then transform the vertex back. That keeps
the `Σx^4` term and the determinant in a comfortable `float` range on the
soft-float CPU.

For a Legendre-style fit (as in the 2025 hybrid), normalise `x` to `[-1, 1]`
first and use `[1, x, (3x^2-1)/2]`; it is algebraically the same quadratic with
better conditioning. That is a refinement, not a different algorithm.

### 3.4 Predictive braking

The current `afdiag_afsearch_task` loop is the perfect place for this. Every
sample after the far-leg arming:

```c
/* last 6 samples, x relative to now */
float tp, sp;
if (n >= 6 && af_fit_quadratic(dt, ds, 6, &a, &b, &c))
{
    tp = -b / (2 * a);
    sp = c - b * b / (4 * a);
    if (tp > 0 && tp <= brake_ms && sp > peak * 1.05f)
        stop = 1;
}
```

`brake_ms` is not a constant to guess. Measure it:

1. run the sweep and stop at fixed delays before/after the sharpness peak;
2. log the settled sharpness after each stop;
3. fit the settled peak vs stop time; the best stop delay is the point where
   the lens would coast into the peak;
4. set `brake_ms = command latency + best stop delay` and store it per lens /
   zoom.

The existing post-stop sampling (500 ms) already gives the data for step 2. Add
`pred_t`, `pred_s`, `brake_ms` and `stop_reason` to `AFSEARCH.LOG` so the
offline replay can score the predictor.

A good default for the first test is `brake_ms = 120…200 ms`; the MPU
acknowledges in 20–40 ms, but the lens coast is unknown. Do not enable the
predictor until the first trace is captured.

### 3.5 Sequential peak detection

The fixed 80 % drop and 120 ms dwell become a statistical test. Estimate the
sample noise from the first differences of the armed-phase trace:

```c
float sigma = af_mad(v[i] - v[i-1], n);   /* robust, no sqrt needed for scale */
sigma = MAX(sigma, 1.0f);
```

Then either a CUSUM:

```text
g = max(0, g + (peak - s)/sigma - nu)
stop if g > h
```

with `nu ≈ 0.5`, `h ≈ 4…6`, or a slope test:

```text
fit a line to the last 4 samples
stop if slope < -k*sigma && s < peak*0.98
```

The CUSUM version adapts to light and texture: in a noisy scene it needs more
evidence; in a clean scene it stops sooner. It also replaces the arbitrary
120 ms dwell with a probability-like threshold.

### 3.6 Brent / golden-section

These were waiting for the relative drive; the committed `AF drive` now
provides it (pending field validation). The clean design is a **pure search
loop** that calls the actuator:

```c
/* returns the position with the best score; score is maximised */
static int af_brent(struct af_actuator *a, int lo, int hi, int tol, int *best);
```

Implementation choices:

- positions are signed pulse counts relative to the search start; convert to
  relative moves with `move_rel(p - current)`;
- cache evaluations: if the lens is already at `p`, do not move;
- after every move, wait for settle (fixed `settle_ms` first, then a stability
  check);
- maximum 8–10 evaluations, then return the best seen position;
- if the score is flat or the actuator returns an error, abort to the full
  sweep;
- golden-section is the simpler fallback if Brent's parabolic step is unstable
  on quantised lens positions.

Brent's algorithm is standard; implement from the classic description rather
than copying an Apache-2.0 file. Burkardt's LGPL C version is a useful
cross-check for the bracket logic and tolerance handling.

### 3.7 DFD: treat it as a calibrated coarse estimator

The previous note listed DFD as a possible one-step jump. Implementation
reality is more subtle:

- two frames at different focus positions give a **relative blur** (the
  spectral ratio cancels scene texture);
- turning that into an absolute distance to the focus plane needs a calibrated
  `blur ↔ focus-error` curve **and** a way to resolve the sign/side ambiguity;
- a two-frame estimate can be ambiguous when both frames are on the same side
  of focus; a third sample or the coarse `focus_dist` prior is needed.

So implement DFD as a **coarse estimator**, not a guaranteed one-step solver:

1. capture A at `p0`, move a known `Δp`, capture B (optionally C at `2Δp`);
2. compute band energies at two scales; use
   `r = log(E_hi(B)/E_hi(A)) - log(E_lo(B)/E_lo(A))` as a texture-cancelling
   relative-blur proxy;
3. map `r` to `|p* - p0|` with a calibration table measured on a textured
   chart;
4. use `focus_dist` and the direction of increasing sharpness to choose the
   branch;
5. move to the estimate and run one parabolic refinement.

This is still valuable: it can replace a coarse scan with 2–3 moves. But do not
ship it before the calibration table exists. The classical DFD forward model
and the Canon mean-spectral-ratio patent are the references.

### 3.8 Kalman tracking for video

Once bounded moves work, the focus plane position can be tracked:

```text
state: p (focus position), v (velocity)
predict: p' = p + v*dt, v' = v
update:  K = P / (P + R)
         p = p' + K*(z - p')
         v = v' + K2*(z - p' - ...)
```

`z` is the fitted peak position from the local curve fit. Only move when the
predicted blur exceeds the depth-of-field deadband. This is the piece that
makes continuous AF possible without a full search every time.

### 3.9 Confidence gate

Do not accept every peak. Combine independent evidence:

| Evidence | Good sign | Cheap test |
|---|---|---|
| Peak prominence | peak far above start | `peak > 1.3 * start` |
| Fit quality | low residual, `a < 0` | `r2 > 0.8` |
| Metric agreement | `s_hi`, `q`, `m` peaks agree | relative positions within DOF |
| DFD agreement | DFD estimate near fitted peak | within 1–2 depth-of-field steps |
| Scene texture | `emax` and `g` above noise | existing `emax`/`q` limits |
| Distance consistency | `focus_dist` plausible | not `65535` at close subject |

The acceptance rule can be a weighted score; the weights are tuned on the
offline replay, not in the field. A starting point:

```c
float prom  = clamp01((peak - start) / (0.5f * start));   /* 0 at no gain */
float fit   = clamp01((r2 - 0.6f) / 0.4f);
float agree = clamp01(1.0f - fabsf(peak_s - peak_m) / dof_steps);
float dfd   = have_dfd ? clamp01(1.0f - fabsf(dfd_pos - fit_pos) / (2*dof_steps)) : 0.5f;
float score = 0.35f*prom + 0.25f*fit + 0.20f*agree + 0.20f*dfd;
if (score > 0.70f) accept;
```

If the score is low, fall back to the next pipeline stage, then to Canon AF.

---

## 4. Combined controllers

"Combo of many" should mean a **cascade with gates**, not running every
algorithm at once. Three useful pipelines share the components above.

### 4.1 Pipeline A — current command only (full sweep)

```text
start sweep
  -> arm on far leg (focus_dist rises)
  -> per sample: multi-metric measure, peak tracking, quadratic predictor
  -> stop when predictor says the peak is within brake_ms
     or CUSUM says the peak has passed
  -> keep sampling 300-500 ms after stop
  -> confidence gate
  -> low confidence: retry once / Canon half-press
```

This is the pipeline that works with the confirmed full-sweep command today. It
cannot remove the near leg; it improves the stop and the reliability.

### 4.2 Pipeline B — the current `AF drive (MPU)` and v1

The repo contains `afdiag_afdrive_task()` and `afd_run()` (commit
`7be6799`, 2026-10-19; line numbers will move as the code changes). The loop is
a useful v0:

- opens a `(9,0x25,16)` session;
- sends raw `(9,1,5)` moves `[hi lo 07 00 00]`, waiting for `FOCUS_DONE` and a
  minimum time, then settling and averaging two sharpness samples;
- baseline sample, then a **direction probe**: +80 pulses; if sharpness is
  below 95 % of baseline it tries −160; if the result is within ±5 % it tries
  the other side and keeps the better one;
- **coarse climb** in ±80-pulse steps until two moves are below 85 % of the
  best or three moves change by less than 3 %;
- returns to the coarse best, then **fine climb** in ±20-pulse steps until one
  move is below 95 % of the best; returns to the best, stops, closes the
  session, and logs `ML/LOGS/AFDRIVE.LOG`.

That is a solid first actuator test and a real hill-climb AF. Before trusting
it, run the `mpu_lab` **Drive test (9,1)**: ±100 and ±300 pulses should produce
a visible, monotonic sharpness change. If the lens does not respond, the AF
drive is climbing on noise and no algorithm change will help.

The limitations are exactly the ones the literature addresses:

| v0 behaviour | Why it costs time / accuracy | v1 change |
|---|---|---|
| fixed 80/20 pulse steps | not tied to depth of field; too small is slow, too large overshoots | measure pulses-per-DOF and set coarse ≈ 1.5×DOF, fine ≈ 0.3×DOF |
| two-point direction probe | noise can pick the wrong side; never tests the better opposite side if the first step improves | 3–5 point linear fit, or test both sides once and choose by slope |
| stop after 2 drops / 3 flats | needs overshoot and a return; plateau is scene-dependent | fit a quadratic to the last 6–10 points and jump to the vertex; stop when the vertex is within the current DOF |
| one sharpness value | exposure/luma changes can look like defocus | normalise by `g`/luma and use a median of 3; log `q = s/g` |
| fixed drop thresholds | hand-tuned for one scene | CUSUM or dynamic slope threshold from the last 3 slopes |
| return-to-best + fine climb | extra moves and reversal/backlash | compute the peak from the fit; one verification move; Brent only if the fit is unstable |
| no confidence gate | accepts a local maximum or noise | prominence + fit residual + `emax` gate; fallback to sweep/Canon |
| no movement verification | if `(9,1,5)` silently does nothing, it climbs on noise | first probe must change sharpness, `g`, or `focus_dist`; otherwise abort with a clear message |
| no end-stop guard | a long run toward the end stop can stall | abort on repeated `FOCUS_DONE` error, no movement, or `dist` at infinity |
| per-move wait ~350–450 ms | 25 coarse moves ≈ 8–10 s | use `FOCUS_DONE` early (already partly done), shorter settle, and the fit to cut moves to 5–8 |

The v1 loop keeps the command layer and replaces only the decision layer:

```text
baseline (median of 3)
  -> movement verification / end-stop check
  -> 3-5 point direction estimate
  -> coarse climb with calibrated step and dynamic slope threshold
  -> quadratic/Gaussian fit over the last 6-10 points
  -> jump to the fitted vertex
  -> verification sample + confidence gate
  -> if not accepted: Brent inside [vertex - 2*step, vertex + 2*step]
  -> return/stop/close, log the fit and the confidence
```

This is the libcamera coarse/fine state machine, the 2025 hybrid's direction
and slope logic, and OughtaFocus's Brent in one loop. The fit removes the
back-and-forth; Brent is only the safety net. The implementation can keep a
menu switch between "v0" and "v1" so the field logs compare directly.

### 4.3 Pipeline C — video / continuous

```text
every 100 ms (or every LV frame):
  measure
  Kalman predict/update
  if predicted blur < deadband: do nothing
  else: small relative move, re-measure
  if focus error grows or scene change: reacquire with Pipeline B/A
```

Full sweep is never used here. If the AF drive is not validated, keep Canon's
half-press for video.

### 4.4 Portfolio selector

Choose the pipeline from a few observable facts, not from a learned model at
first:

| Condition | Pipeline |
|---|---|
| no relative/bounded drive | A |
| relative drive + static scene + texture | B |
| relative drive + low light / weak texture | B with median-of-3 and GP/BO fallback |
| video or recording | C |
| repeated failure / scene change | A once, then Canon half-press |
| focus_dist at infinity | refuse / move nearer first |

A later refinement is an **algorithm portfolio**: use the logs to learn a small
decision tree over `(emax, g, luma, zoom, focus_dist, last_success)` that picks
A/B/C and the step sizes. That is a cheap form of meta-learning and does not
require a neural network on the camera.

### 4.5 One controller, explicit phases

The cascade is easiest to reason about as an explicit state machine. The
implementation can start with only the phases that are reachable with the
current commands, and add the rest when the actuator work is done.

```c
enum af_phase
{
    AF_IDLE = 0,
    AF_PRIOR,       /* collect focus_dist / last focus / scene stats */
    AF_DFD,         /* optional calibrated coarse estimate          */
    AF_COARSE,      /* 3-5 points, direction test                   */
    AF_HILL,        /* coarse-to-fine with dynamic slope threshold  */
    AF_FIT,         /* quadratic/Gaussian fit, jump to vertex       */
    AF_VERIFY,      /* one sample + confidence gate                 */
    AF_BRENT,       /* local refinement inside the bracket          */
    AF_SWEEP,       /* full MPU sweep fallback                      */
    AF_TRACK,       /* Kalman tracking for video                    */
    AF_DONE,
    AF_FAILED,
};
```

Transitions:

- `IDLE -> PRIOR` on an AF request;
- `PRIOR -> DFD` if calibration exists, otherwise `PRIOR -> COARSE` if the
  AF drive is validated, otherwise `PRIOR -> SWEEP`;
- `DFD -> COARSE` with a narrowed bracket, or `DFD -> FIT` if the estimate is
  confident;
- `COARSE -> HILL -> FIT -> VERIFY`;
- `VERIFY -> DONE` if the confidence gate passes, otherwise `VERIFY -> BRENT`;
- `BRENT -> DONE/FAILED`; `FAILED -> SWEEP` once, then Canon half-press;
- `TRACK` is entered instead of `PRIOR` while recording / in continuous mode,
  and falls back to `PRIOR` on a scene change.

The important property is that every phase has a **timeout and a fallback**.
No phase may leave the lens driving, and every exit calls `(9,3,1)`.

One more possible prior: the EOS M has Canon Hybrid CMOS AF, so the MPU may
already compute a phase/disparity value. If `(9,0x17,33)` or `(9,0x21,61)`
exposes such a field, it can replace the coarse stage or disambiguate the DFD
branch. That is a reverse-engineering check, not an algorithm change; it is
worth one look before investing in a learned model.

---

## 5. Integration into `crop_rec`

Suggested split, keeping the first version simple and starting from the
existing `afd_*` code:

1. **Actuator layer** — factor `afd_move()` into `af_act_send()`,
   `af_act_move_rel()`, `af_act_search()`, `af_act_stop()`,
   `af_act_settle()`. The current `afd_move()` already has the `FOCUS_DONE`
   wait and settle logic; keep that behaviour and add the guards.
2. **Measurement layer** — extend `afdiag_sharpness_ex()` or add
   `af_measure_multi()` next to it; do not change the existing Auto refocus
   metric until the new one is validated.
3. **Search core** — split `afd_run()` into `afd_run_v0()` and `afd_run_v1()`
   (or one function with a policy enum) so the old algorithm remains available
   for A/B logs. Add the phase enum `AF_PHASE_PROBE / COARSE / FIT / VERIFY /
   BRENT / SWEEP / DONE`.
4. **Logging** — keep `AFDRIVE.LOG` for the drive loop; add the fit fields
   `pred_t`, `pred_s`, `brake`, `fit_r2`, `confidence`, `reason`. `AFSEARCH.LOG`
   stays for the full-sweep pipeline.
5. **Menu** — keep `AF drive (MPU)` as v0/v1 and add a policy choice rather
   than replacing the old action.
6. **Guards** — add `af_search_active` (or reuse `afdiag_afdrive_busy`) to
   `afdiag_mpu_busy()`, `aref_poll()` and the menu entry preconditions. Stop
   the search if LV is lost, recording starts, the menu opens, the lens reports
   infinity, or no movement is observed after the first probe.
7. **Task** — keep `task_create("afdiag_afdrive", 0x1e, 0x3000, ...)`. Run it
   one-shot in a separate task, as the current action does.

Do not put the new code in the generic `lens_focus()` path: it clamps the step
size to ±3 and refuses manual focus, which is wrong for the MPU search. Use the
raw send wrapper and enforce the safety rules there.

### Log format

For `AFDRIVE.LOG`, extend the per-move line:

```text
ms phase cmd pos s_hi s_mid s_lo g luma emax q m dist done_val done_ms ret pred_t pred_s r2 conf
```

For `AFSEARCH.LOG`, extend the per-sample line similarly:

```text
ms s_hi s_mid s_lo g luma emax q m dist pos phase pred_t pred_s brake reason
```

Keep the existing `s`/`g` columns for backward compatibility, or bump the file
version in the header. The replay harness needs at least: time, raw metrics,
distance, position, phase, and the stop reason.

---

## 6. Offline replay and simulation

Do this before more camera time. The log is small and the policies are pure
functions of the sample stream.

### 6.1 Replay harness

Host-side toolbox: `numpy` for the trace arrays, `scipy.optimize.brent` /
`minimize_scalar` as the reference Brent implementation,
`scipy.optimize.least_squares` for the Legendre/quadratic cross-check,
`opencv-python` or `scikit-image` for Laplacian/FFT metrics, and
`matplotlib` for the per-run plots. These are only for analysis; the camera
implementation remains the C code in `crop_rec`.

```python
def load_afsearch(path):
    samples = []
    header = {}
    for line in open(path):
        if line.startswith("#"):
            # parse key=value header
            ...
        else:
            ms, s, g, luma, dist, phase = line.split()
            samples.append(dict(t=int(ms), s=int(s), g=int(g),
                                luma=int(luma), dist=int(dist), phase=phase))
    return header, samples

def replay(samples, policy):
    peak = 0
    for i, x in enumerate(samples):
        if policy.stop(x, samples[:i+1]):
            return i, samples[i]
    return len(samples)-1, samples[-1]
```

Policies to compare on the same trace:

- current 80 %/120 ms;
- oracle (stop at the sample with the highest `s`);
- quadratic predictor with several `brake_ms`;
- CUSUM with several thresholds;
- combined predictor + CUSUM.

Metrics: stop time, sharpness at stop, settled sharpness from the post-stop
samples, overshoot, distance error. This is how the thresholds should be
chosen.

### 6.2 Synthetic simulator

Before the first trace, a simple simulator is useful for sanity checks:

```text
position(t): piecewise constant velocity with acceleration limit
sharpness(p): Gaussian in p, plus noise and a slow exposure drift
command delay: 30 ms; settle: 100 ms
```

Run 1000 random subject positions and noise levels for each policy and compare
time-to-focus and success rate. The absolute numbers are not evidence — the
assumptions dominate — but the relative ranking of the policies is useful for
deciding what to implement first.

---

## 7. Test matrix and acceptance

Minimum matrix for the first implementation round:

| Factor | Values |
|---|---|
| Distance | 0.3 m, 1 m, 3 m, near-infinity |
| Lens | EF-M 18-55 at 18/35/55 mm, EF-M 22 mm |
| Zoom | x1, x5 |
| Aperture | wide open, f/8 |
| Light | bright, indoor, low light |
| Texture | detailed chart, flat wall, high-contrast edge |
| Mode | photo LV, movie LV, recording (should refuse) |
| Policy | full sweep, AF drive v0, AF drive v1 |

For every run record: time to stop, settled sharpness, overshoot, final
`focus_dist`, number of moves, command errors, `FOCUS_DONE` value, and whether
the confidence gate accepted. For `AF drive`, also record the commanded
position at each move and the sharpness at the start/end of the run.

Acceptance for Pipeline A: equal or better settled sharpness, stop 200–500 ms
earlier, no increase in false stops. For Pipeline B: 5–8 moves, under about
1.5 s, repeatable within depth of field. For Pipeline C: no visible hunting on
a static scene and bounded correction on a moving subject.

---

## 8. Implementation order

1. **Phase 0:** capture one `AFSEARCH.LOG` and one `AFDRIVE.LOG` with the
   current build; write the replay harness; add the new log fields. The
   `AFDRIVE.LOG` is already the most valuable trace because it contains the
   move number, commanded position, sharpness and `FOCUS_DONE` value.
2. **Phase 1:** validate `AF drive (MPU)` on the camera. Does the lens actually
   move for `(9,1,5)` with speed `0x07`? Do the moves accumulate? Is the
   direction probe reliable? Does the fine phase return to the coarse best?
   Log every run; do not change the algorithm until this is answered.
3. **Phase 2:** characterise the actuator: direction, speed, latency,
   repeatability, pulse count (`0x8005001E`), and **pulses-per-depth-of-field**.
   Move in steps until sharpness falls to about 80 % of its peak; that distance
   sets the coarse step, the fine step and the Brent tolerance.
4. **Phase 3:** implement the v1 drive loop: multi-metric measurement,
   direction from 3–5 points, dynamic slope threshold, quadratic/Gaussian fit
   and jump, confidence gate, Brent fallback. Keep v0 selectable for A/B logs.
5. **Phase 4:** full-sweep predictive stop (Phase 1 of Section 7) if the sweep
   is still needed as a fallback; then DFD calibration and Kalman tracking for
   video; only then a learned or GP-based fallback.

A useful rule: **each phase must beat the previous one on the replay harness
before it is flashed.** The camera is the slowest and riskiest test
environment; the log is the fast one.

---

## 9. Source map

Internal:

- [`EOSM-AF-ALGORITHMS.md`](EOSM-AF-ALGORITHMS.md) — algorithm families,
  cost model, references.
- [`EOSM-MPU.md`](EOSM-MPU.md) — command protocol and field history.
- [`EOSM-MPU-HANDOVER.md`](EOSM-MPU-HANDOVER.md) — current status.
- [`EOSM-AF.md`](EOSM-AF.md) — Auto refocus and the sharpness metric.
- `modules/crop_rec/crop_rec.c` — `afdiag_sharpness_ex()`,
  `afdiag_afsearch_task()`, `afdiag_afdrive_task()`, `afd_run()`,
  `afdiag_mpu_build()`.
- `modules/mpu_lab/mpu_lab.c` — `MPU_SEND`, raw sequences, `Drive test (9,1)`.
- `src/focus.c`, `src/lens.c` — `lens_focus()`, step limits, focus stacking.

External implementations:

- libcamera contrast AF patch:
  <https://lists.libcamera.org/pipermail/libcamera-devel/2023-March/037336.html>
- libcamera Raspberry Pi focus:
  <https://chromium.googlesource.com/chromiumos/third_party/libcamera/+/fd2b55cb3f2b50e273e01f422c251c18ffa441ef/src/ipa/raspberrypi/controller/rpi/focus.cpp>
- Micro-Manager `OughtaFocus`:
  <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/autofocus/OughtaFocus.java>
- Micro-Manager `Autofocus.java`:
  <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/autofocus/Autofocus.java>
- Micro-Manager `SimpleAutofocus/score.cpp`:
  <https://github.com/openspim/micromanager/blob/7aef2e992ba24b87ab0de54a5c6beb7bb4e8104b/DeviceAdapters/SimpleAutofocus/score.cpp>
- Enhanced mountain climbing paper:
  <https://www.biorxiv.org/content/10.1101/2025.03.14.642633v1.full>
- Enhanced mountain climbing code:
  <https://github.com/jyt368/Autofocus>
- Brent reference implementation:
  <https://people.math.sc.edu/burkardt/c_src/brent/brent.html>
- DFD reference:
  <https://github.com/hollyjackson/dfd>
- Modern learned DFD:
  <https://github.com/princeton-vl/FOSSA>
- Canon mean-spectral-ratio patent:
  <https://patents.google.com/patent/US9836855B2/en>

Licence notes:

- libcamera code is LGPL-2.1-or-later: reference and link, do not paste into
  GPLv2 sources without checking compatibility.
- Micro-Manager and `SimpleAutofocus` code is BSD: compatible with GPL; keep
  the copyright notice if ported.
- Burkardt's Brent is LGPL: reference or reimplement.
- `jyt368/Autofocus` has no explicit licence: use the paper, not the code.
- `hollyjackson/dfd` is MIT; `FOSSA` is BSD-3; both are host-side references.
