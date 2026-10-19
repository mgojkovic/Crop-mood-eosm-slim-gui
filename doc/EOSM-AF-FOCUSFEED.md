# Canon per-frame AF feed mining: is `(9,0x17,32)` a usable defocus measurement?

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Camera:** EOS M 2.0.2, EF-M 18-55
STM · **Task:** task-11, follow-up to [EOSM-AF-NOVEL.md](EOSM-AF-NOVEL.md) idea N1.
**Verdict up front:** **no** — the existing logs do not establish any payload field as a
defocus proxy. Word `A` (bytes 4..7) is the only un-decoded candidate and it is *most
strongly associated with scene brightness* (Spearman +0.80 across static scenes), not with
defocus. The result is a falsification of "the Canon defocus number is minable from the
recovered logs", not of the existence of such a number: **no controlled raw lens move was
ever logged together with the feed**, so the decisive test remains a dedicated one-session
protocol. N1 stays the top roadmap idea, but its next step is that session, not more
mining.

Reproduce:

```
python3 contrib/af_novel/analyze_focusfeed.py            # ~4 s, stdlib only
python3 contrib/af_novel/analyze_focusfeed.py --json /tmp/ff.json
python3 contrib/af_novel/analyze_focusfeed.py --s-ratio 1.3 --luma-tol 1.03   # stricter windows
```

---

## 1. Corpus, and why it is smaller than the file count suggests

| | |
|---|---|
| `AFDIAG*.LOG` files recovered | **106** (scratchpad `card*/`, incl. rotated `AFDIAG1..29`) |
| unique by content | **26** (80 are repeated card copies; deduplicated by md5) |
| logs containing `(9,0x17,32)` | **6**, of which 5 have ≥50 records |
| `(9,0x17,32)` records | **3 204** (3 097 in the active format) |
| records with a fresh sharpness sample (≤120 ms) | 2 881 |
| logs where `AF diag log = ON + MPU` | these same 6 — the hook is only installed in that mode |
| **raw speed-07 `(9,1,5)` moves inside feed logs** | **0** |
| ML `lens_focus` no-op `(9,1,5)` messages | 145 (all `[hi lo FF FF 00]`, the form that does not move this lens) |

This is the crux: the AF feed is only captured when the hook is on, and in every session
where it was on **the lens was never driven by our raw `(9,1,5)` speed-07 command**. The
lens motion visible in these logs is Canon's own AF (half-press, refocus test) and a few
test/sweep episodes. The two workstreams — "log the MPU traffic" and "drive the lens with
raw pulses" — were never run in the same session, so there is no known-ground-truth move
to correlate the payload against.

Additional details that matter:

- The hook captures **12 payload bytes** (`w1..w3`), not 16: `w0` is the message header
  (`grp<<16 | op<<8 | len | drop<<24`). Bytes 12..31 of the 32-byte payload — including the
  candidate "u32 C" — are **not in any log**. The full 32-byte default payload only exists
  in the QEMU spell `contrib/qemu/eos/mpu_spells/EOSM.h`; 700D has several captured copies
  in `700D.h`.
- `@SHARP` logs only `s = w0`, **mean luma = w1**, `samples = w2`, `max edge = w3`.
  Contrary to the task note, `g` is **not** in `@SHARP` (it exists only in `@SWEEP` /
  `@MPUTEST` records, which do not overlap the feed). The analysis therefore uses
  `s`, `luma` and `max_edge` as the context variables.
- Session headers in feed logs: `cont_af_movie=0` everywhere (required by all analysis
  below); zoom x5 except a few x1/x10 records. `small_hacks=2` in AFDIAG26 only.

## 2. Method

`contrib/af_novel/analyze_focusfeed.py`:

1. deduplicates the 106 files by content, parses headers and every line;
2. extracts every `(9,0x17,32)` message, decodes the 12 payload bytes and pairs it with the
   most recent `@SHARP`;
3. classifies records as **active format** (bytes 1, 3, 6, 10 = 0) or the **constant
   default format** (`fe 03 fe 01 2e e8 0f 00 fe ff 1f 00 ...`, the same as the QEMU EOSM
   spell; 103 records);
4. builds **static windows** — neighbourhoods with no Canon-AF/zoom/command markers within
   ±1 s, `af=0`, `hs=0`, `rr=0`, constant zoom/`pos`/`dist`, s stable within 5 %, luma
   within 3 % — to measure field noise and cross-scene associations;
5. builds **focus-change windows** — exposure/zoom/recording constant, s range ≥20 %
   (default; 30 % strict), luma range ≤5 % (default), non-overlapping — these are the only
   places a defocus signal could show up in this corpus; they are **not** labelled by
   cause, so their biggest confound is Canon's own AF;
6. builds **crossing windows**, where s has an interior peak/trough, to separate a
   magnitude-of-defocus field (low at focus, high on flanks = V-shape) from a
   contrast/sharpness field (inverted V);
7. tests every field (all 12 bytes, u16/u32 interpretations, `w`, `h`, `w*h`, `A`, `B`,
   `A/B`, `A-B`) against `s`: Spearman ρ per window, bootstrap 95 % CI of the median,
   exact sign test, permutation p (1000 within-window shuffles), **rank partial
   correlation controlling for the frame-geometry confound `w*h`**, per-log medians
   (leave-one-log-out proxy), and the V-shape statistic per log.

### Sample sizes (state them plainly)

- 59 static windows / 413 feed records (4 logs);
- 32 focus-change windows / 183 feed records, 23 crossing windows;
- only 4 logs contribute focus-change windows; one half-press window in the relaxed set.
- These are small. All p-values are within-corpus only; nothing here is a measurement of
  the camera's defocus behaviour.

## 3. Established

1. **The feed is a real, periodic state stream.** In static periods the fields are stable:
   within-window relative sd is **1.4 % for `A`**, **3.3 % for `B`** (and the ML metric's
   own s-noise in the same windows is 1.2 %, matching the measured 1 %). So `A`/`B` are
   not noise; they encode something.
2. **`B` (bytes 8..11) is a scene-contrast / sharpness-like evaluation value.** Across
   static windows from different scenes it correlates with the ML sharpness
   ρ=**+0.90** and with max edge ρ=+0.76; across all records ρ(B, s)=+0.66. This matches
   what ML core already assumes on other cameras (`PROP_LV_FOCUS_DATA` words 2..4 are used
   for the focus-confirmation graph). It is not new information about defocus.
3. **`A` (bytes 4..7) is strongly brightness/exposure-like.** Across static scenes
   ρ(A, luma)=**+0.80**, ρ(A, s)=+0.13, ρ(A, max edge)=+0.07. Across all records
   ρ(A, luma)=+0.79. That is the dominant association of the only word ML does not use.
4. **`w`,`h` (first u16 pair) are not confirmed geometry.** They form a ~2.0:1 rectangle
   (median 62×30 in the active format at x5), the constant/default format has the
   1022×510 sentinel (also 2:1), and the medians differ by zoom state (x5 60×30;
   x1 and x10 ≈150×70). But in "nothing is happening" static windows they still fluctuate
   by **11–12 % relative sd**, which a fixed AF frame should not do. Either it is not a
   fixed frame or Canon updates it per frame; either way it is a **confound** for `B`
   (ρ(B, w·h)=0.85 overall, +0.83 across static scenes).
5. **The feed's own format is two-state**: 3 097 active records and 103 constant default
   records. The default record is exactly the QEMU EOSM `PROP_LV_FOCUS_DATA` payload
   (`fe 03 fe 01 2e e8 0f 00 ...`). A decoder must handle both.

## 4. Plausible (not established)

- **`A` may also carry an AF-state term on top of its brightness scale.** In focus-change
  windows with 5 % luma control, A's monotonicity against s is null (median ρ=−0.09,
  p=0.41) and its V-shape statistic is +0.27 sd but **sign-inconsistent per log**
  (+0.11, +0.89, +2.18, −0.25). If `A` were `k·brightness + defocus_term`, AE co-movement
  could mask the defocus term in this corpus. Only a fixed-exposure, known-move session
  can separate the two.
- **The payload may be a per-frame *evaluation vector*, not a physical defocus.** `B` is
  contrast-like, `A` is brightness-like; `w`/`h` may be an evaluation-block size. That is
  consistent with an AF/AE evaluation feed. A phase/defocus number, if present, would be
  in the **unlogged bytes 12..31** or in a mode not exercised in these sessions.
- **`A/B` median ρ(s) = −0.57** looks encouraging in isolation, but it is explained by
  A~luma and B~s; its partial correlation controlling `w*h` is only −0.24 and two per-log
  medians are positive. Do not build on it.

## 5. Falsified

- **"Any payload field is a usable defocus proxy from the existing logs."** No field
  passes the pre-set gate (|median ρ| ≥ 0.6, permutation p ≤ 0.05, no sign flips across
  logs, partial ρ ≥ 0.5 after removing geometry). `A` fails on all four, `B` fails on
  robustness/sign.
- **"Field A is a defocus magnitude"** (the reading of the single-card extract in
  [EOSM-AF-NOVEL.md §3 N1](EOSM-AF-NOVEL.md) and `focusfeed_probe.py`). The wider corpus
  shows A tracks brightness (ρ=0.80) with no consistent relation to sharpness; the
  original A-vs-1/s correlation (+0.60) came from one Refocus-test log where AE was also
  changing.
- **"More log mining will settle it."** It cannot: there are zero controlled raw moves,
  only 12 of 32 payload bytes, 4 Hz sampling, and Canon's own AF/AE/zoom runs concurrently.
  Additional mining would add confounded windows, not information.

## 6. Best candidate (and why it is still not usable)

**Word `A`, bytes 4..7** (the only 32-bit field ML core does not consume). Current
evidence: stable in static periods (1.4 %), brightness-dominated (ρ=0.80), weak and
inconsistent focus response (V-shape +0.27 sd, per-log signs mixed). It is the field to
test in the dedicated protocol — not a field to build an algorithm on.

Nothing else is close: `B` is a known contrast/sharpness evaluation, `w/h`/`w*h` are a
fluctuating 2:1 rectangle, `A-B`/`A/B` are combinations of the two confounded fields, and
bytes 12..31 are invisible.

## 7. Biggest confounds that remain

1. **No ground-truth lens motion**: no raw speed-07 moves in feed logs; Canon-AF windows
   are simultaneously the stimulus and a state change in Canon's AF engine.
2. **AE/brightness**: `A` is 80 % brightness-linked; luma control in windows is only 5 %.
3. **Canon AF/zoom state**: `w,h`/`w*h` change with AF/zoom activity and are correlated
   with `B`; the partial-correlation control is imperfect.
4. **Missing bytes**: 20 of 32 payload bytes never logged; a defocus field may live there.
5. **Sampling**: the hook logs at most one record per 250 ms per (group, op), so the feed is
   seen at 4 Hz, not per frame, and `@SHARP` is not synchronous.
6. **Small corpus**: 26 unique sessions, 4 contributing logs, 32 windows; per-log sign tests
   are weak evidence either way.

## 8. Next session: minimal decisive protocol (supersedes the N1 protocol in the novel-ideas doc)

Goal: **separate defocus from brightness and Canon state with known lens motion.**
This needs one small logging change from the crop_rec workstream (not done here): log all
**32** payload bytes of `(9,0x17,32)` per message and every frame, not the 12-byte/4 Hz
hook; keep `@SHARP` at its normal rate. Everything else is already in the firmware.

Setup: tripod, static high-contrast chart filling the centre AF area, **manual exposure
(ISO/shutter/aperture fixed), AF lock/MF, IS off**, `cont_af_movie=0`, x5 crop preset,
`AF drive monitor: Keep open`, `AF diag log: ON + MPU`.

Procedure (~30 min, one field session):

1. wait 10 s, ten consecutive **Canon half-press AF** runs at the same distance
   (each followed by a 3 s settle) — labels the "Canon AF active" state;
2. from a known start, six **raw speed-07 moves** `+200, +100, +40, −40, −100, −200`
   pulses, 2 s settle each, 5 repetitions, no half-press in between (ML moves only);
3. move to a second distance (1 m vs 2 m) and repeat steps 1–2;
4. repeat at x10 if reachable without dropping the preset (note whether it is);
5. optional: one run with the chart defocused by hand between the two distances.

Analysis, with `analyze_focusfeed.py` extended to the new logs (or a short follow-up
script): for each record, the **commanded pulse offset from the start of the move sequence
is known ground truth**. Accept a candidate field iff:

- **monotone with |commanded pulses|** through focus (V-shape; sign of flank−centre
  consistent across repetitions and both distances), **and**
- the effect survives **fixed exposure** (luma range ≤2 %) and **no half-press**, **and**
- its `ρ` with luma (across distances/exposures) is **< 0.3**, **and**
- it does not track `w·h` (partial ρ ≥ 0.6).

Expected signal: a usable defocus field should change by ≫ its static sd (1.4 % for A)
across ±200 pulses with a clear sign flip when the focus plane is crossed, on every
repetition.

## 9. Kill criteria for N1 after that session

Stop pursuing a decoded defocus number if, at fixed exposure and static scene:

- no field of the 32 bytes varies with a known ±200-pulse move by more than 1 static sd
  (except monotone-through-motion artifacts such as a blur/continuity counter), or
- every field that varies is explained by `w·h`/luma, or
- the only varying fields behave like `B` (contrast evaluation) and are also present in
  the half-press-off state.

If that happens, N1 is downgraded: the feed is an evaluation/status stream, and the
roadmap should move to N2 (two-position differential multi-band jump, already validated
offline) and N13/N15a (pulses-per-DOF, MPU RAM counter) as the measurement foundation.

## 10. Consequence for the roadmap

- **N1 stays the top information-leak idea**, but only because the dedicated protocol has
  not been run. Log mining is complete and negative; do not spend another session on it.
- **N2 is unaffected and should proceed in parallel** — it needs no protocol change and
  already has cross-scene offline validation (MAE 10–20 pulses, leave-one-scene-out).
- **No algorithm should depend on the AF feed until a field passes §8.**
- The one cheap extra to add to the same session: **N15a MPU RAM diff** (a motor-step
  counter read with the already-open monitor). If that succeeds, it is worth more than
  the feed for position, and it makes N2's calibration easier.

## 11. Reproduce / outputs

```
python3 contrib/af_novel/analyze_focusfeed.py
```

prints the corpus table, payload-format statistics, static-window noise and cross-scene
correlations, focus-window field tests with CIs/p-values, V-shape per log, per-log sign
consistency, and the verdict. The numbers quoted above are the default run on the
recovered scratchpad corpus (26 unique logs, deterministic, ~4 s). `--json` writes the
per-field statistics for downstream use. The companion one-card extract and its protocol
live in [`contrib/af_novel/focusfeed_probe.py`](../contrib/af_novel/focusfeed_probe.py)
and [`samples/`](../contrib/af_novel/samples/); this note supersedes that protocol.

## 10. Logging change implemented (task-12: all 32 bytes of `(9,0x17,32)`)

**Status:** implemented in `modules/crop_rec/crop_rec.c`, **compile-checked only** (no camera attached).
Logging-only: no search, trigger, landing or timing change; hook rate unchanged (≤ 4 Hz per
`(grp, op)`, `AFDIAG_MPU_MIN_MS = 250`); no new task and no new buffer.

### 10.1 What changed

The `@MPU` hook (`afdiag_mpu_cbr`, the patched ICU→MPU packet builder at `0x38C4`) used to copy only the
first 12 payload bytes of every message. For the Canon-AF data feed **`(9, 0x17, 32)`** it now captures all
32 bytes:

- the first 12 bytes are still in the four `w=` fields exactly as before (backward compatible; existing
  parsers keep working, old logs parse with `b32 = None`);
- the remaining 20 bytes go into one extra ring entry (`AFD_EV_MPUF`, written under the same `cli()` as
  its `@MPU` record, so the pair is always adjacent) and `afdiag_flush` folds it into the same `@MPU` line
  as ` feed=32B w4=...,w5=...,w6=...,w7=...,w8=...`;
- the log header now ends with ` feed=32B` (self-describing: a log without it was written by the old
  firmware and only has 12 payload bytes);
- every other message type keeps the 12-byte capture. An orphan `@MPUF` line (only possible if a ring
  wrap separates the pair) is printed as its own line rather than dropping bytes silently.

Payload mapping (all payload dwords are LSB-first, byte 0 in the low byte of its dword):

| field | payload bytes | meaning |
|---|---|---|
| `w1` | 0..3 | first payload dword (`w,h` rectangle in the active format) |
| `w2` | 4..7 | second dword (field `B`, the known contrast/sharpness evaluation) |
| `w3` | 8..11 | third dword (field `A`) |
| `w4` | 12..15 | **new:** fourth dword |
| `w5` | 16..19 | **new:** fifth dword |
| `w6` | 20..23 | **new:** sixth dword |
| `w7` | 24..27 | **new:** seventh dword |
| `w8` | 28..31 | **new:** eighth dword |

(`w0` is the header word: `grp << 16 | op << 8 | len | dropped_repeats << 24`.)

### 10.2 Old vs new line format

Old (still used for every non-feed message, and by older logs):

```
00001234 @MPU n=16 w=00091720,02000501,012c3f00,00000000 z=5 rr=0 hs=0 af=2 pos=1234 dist=456
```

New (feed messages only; the first part is byte-identical, the extension is appended at the end):

```
00001250 @MPU n=16 w=00091720,02000501,012c3f00,00000000 z=5 rr=0 hs=0 af=2 pos=1234 dist=456 feed=32B w4=11121314,w5=15161718,w6=191a1b1c,w7=1d1e1f20,w8=21222324
```

Header: `# AFDIAG ON t=... ... zoom=x5 feed=32B`.

### 10.3 Volume at 4 Hz

Measured on the exact format strings with representative values:

- old line: 94 bytes → 22.6 kB/min at 4 Hz (22.0 KiB/min);
- new line: 163 bytes (69 bytes more) → 39.1 kB/min (38.2 KiB/min), i.e. **+16.6 kB/min**;
- a 30-minute N1 session adds ≈ 1.17 MB of feed lines. `AFDIAG_MAX_FILE` is 1 MB, so the file will rotate
  once (`AFDIAG.OLD` keeps the earlier part) — **copy both `AFDIAG.LOG` and `AFDIAG.OLD`** after the
  session.

### 10.4 @SHARP `g`: not added

`@SHARP` already uses all four stored words (`w0` edge sum, `w1` mean luma, `w2` samples, `w3` max edge) and
there is no spare per-record storage. Appending `g` would need either a larger ring entry (+16 KB RAM) or a
second paired ring entry for a 10 Hz event (a second pairing path plus double ring pressure during
recording); both are outside "cheap and safe" for this logging-only patch. The field session therefore still
pairs `@MPU` with the nearest `@SHARP` `s`/`luma`; `g` remains available only in `@SWEEP`/`@MPUTEST`/`# aref`
records. If a later session shows that the extra bytes need scene-contrast normalisation, the continuation
mechanism from 10.1 can be reused for `@SHARP` (one `@SHARPG` entry per line, 10 Hz).

### 10.5 Analysis after the session

`contrib/af_novel/analyze_focusfeed.py` has been extended minimally: `RE_LINE` now captures the optional
`feed=32B w4..w8` tail, `parse_log` stores `b32` (the full 32-byte payload, or `None` on old logs), and the
"payload structure" section reports how many feed records carry the full payload. No analysis was rewritten.

To test the newly visible bytes after the N1 session, add them as fields next to the existing ones, e.g.:

```python
# in make_fields(): payload bytes 12..31 as u32 / u16 fields
for off in range(12, 32, 2):
    fields['u16@%d' % off] = (lambda b, o=off: int.from_bytes(b[o:o+2], 'little'))
for off in range(12, 32, 4):
    fields['u32@%d' % off] = (lambda b, o=off: int.from_bytes(b[o:o+4], 'little'))
# and make the field accessors use r['b32'] when present (None-safe fallback to r['b'])
```

then rerun the same per-field tests from §6/§8 on `b32` (static-period sd, Spearman vs `s`/`luma`/`w*h`,
V-shape through a known ±200-pulse move). Records that carry only 12 bytes should be skipped for those
fields. The N1 accept/kill criteria are unchanged (§8/§9); the point of this logging change is simply that
bytes 12..31 are no longer invisible.
