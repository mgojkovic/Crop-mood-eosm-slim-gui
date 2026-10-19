# AF retest — patch 6 (2026-10-19 evening)

**Card state:** flashed from `platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip`
after patch 6; verified `ML/modules/crop_rec.mo` md5 `9f3e643f370bf413877b646c0300080e`.
Patch 1–5 module backed up on the Mac at `~/af_logs/flashed-backups/crop_rec-patch1-5-20261019-1938.mo`
(re-flash just that file to roll back the module).

**What patch 6 changes (from your evening session):**
- **Auto refocus now retries the absolute fallback.** Before: after one "no usable peak" attempt the
  camera could refuse to try again for a long time, which is why it "caught focus when it tried, but
  didn't refocus most of the time". Now a failed run re-arms it, up to **3 attempts per blur episode**
  (still bounded by the 4 s cooldown, 20 s failed-scene memory and 3-failure pause).
- **The Refocus test validates its reference landing.** The 55 mm "successful" test was a false pass:
  the reference landing itself was soft (34k) and all five trials then scored 90–141 % against it.
  The test now runs one local-search verification pass on the reference; if it finds >5 % better the
  reference is corrected (`# RT_REF verify local better ref=.. local=..`), otherwise it keeps B07
  (`verify kept`) and the numbers are logged.

**Focal length reports correctly in EXIF but freezes in ML once the MPU monitor opens** (user-confirmed;
this supersedes the earlier "not reported by the camera/lens" note). The lens sends the focal length (the
50 mm + 0.71x speedbooster showed 35 mm in EXIF; the 17–55 shows 18/35/55), but `MpuMonOpen` freezes the
LV_LENS focal-length property — and the camera UI — at the value from before the open. After a zoom with
the monitor open, `focal_mm=`, the Auto Fine step and the `mm` reference bucket stay stale. ML cannot
defeat this; work around it:
- set the zoom **before** the first AF drive / monitor open;
- to change the zoom later: close the monitor (AF diag → monitor setting away from Keep open), change the
  zoom, then run AF drive again to re-open and re-read;
- trust **EXIF**, not `focal_mm=`; set **Fine step manually**: **18 mm → 20**, **35 mm → 12**, **55 mm → 8**;
- the AFDRIVE run header now logs `mm_pre=` and the epilogue `mm_post=` — equal values after a zoom mean the
  property was frozen.

---

## Session A — 18 mm regression (≈4 min)

1. Card in, movie mode, LiveView x5 crop, AF mode, lens 18 mm, textured static subject, tripod.
2. AF diag: monitor **Keep open**, algorithm **B**, sweep **07**, session close **ON**,
   **Fine step 20**, AF diag log **ON**. Auto refocus: **MPU drive**, **Local**, **High**, ON.
   **Movie Servo OFF.**
3. Run **AF drive (MPU)** once and wait out the ~2 s monitor blackout.
4. **Refocus test** — should stay **notrig=0, valid=5, false_pass=0, ok 5/5, false_trig=0**.
   New: look for `# RT_REF verify ...` lines. If `verify local better` appears even here, the
   reference validation is doing its job — note the numbers.

## Session B — 55 mm reference validation (≈5 min, the key new test)

5. Lens to **55 mm**, **Fine step 8**, re-frame a **well-lit, textured** subject so it fills the
   centre AF area (the evening 55 mm scene was dark: luma 43–45, and B's sweep collapsed to
   s ≈ 5–12k). Do not change the camera distance more than needed.
6. **Refocus test.** Expected one of:
   - **honest pass**: a reference that is stable, `ok ≥ 4/5`, `false_trig 0`; or
   - **`# RT_REF verify local better`** with a higher reference and then trials close to it; or
   - a **soft/aborted** reference with `# RT_REF verify kept` / `emax=` low — that is a valid
     "the scene/reference is bad" result, not a pass. Note it and move on.
7. If it passes, repeat once at 35 mm with **Fine step 12** (optional).

## Session C — Auto refocus on blur (≈3 min, the main patch-6 target)

8. Back to **18 mm**, Fine step 20, Auto refocus ON (MPU drive, Local).
9. Point at subject A until it focuses, then move to a **clearly out-of-focus, textured** subject B
   at a different distance and hold it ~5 s. Repeat A↔B 4–5 times, then try a low-texture wall
   briefly (expected: it may try and give up — that is correct, not a bug).
10. Watch the picture and note: does it **attempt** each time B is blurry? Any long stretches where
    it does not? Any hunting (attempt → fail → attempt immediately → fail)?

## Finish

11. Power off, card to Mac, then:

```
cd /Users/mgojkovic/repos/Crop-mood-eosm-slim-gui
python3 contrib/af_test/field_session.py --copy
```

It now also prints the **auto-refocus gate distribution** (expect far fewer `no usable peak` than the
118 in the last session), any `# aref abs re-arm after failed run` lines, and `# RT_REF verify` lines.
Send that output.

## Lens/focal-length checks (separate, 2 min)

- Does the lens **physically zoom** and does the camera show a focal length anywhere (INFO screen)?
- Take one photo and check its **EXIF focal length** on the Mac.
- Reseat the lens / clean the contacts, and if possible try another EF-M lens.
- Report: lens model, whether the UI/EXIF show a focal length, and whether reseating changed it.
  If EXIF is correct but ML logs 18–21, the bug is in ML's LV_LENS decode and we can chase it;
  if EXIF is also missing, it is the lens/body communication and no AF algorithm can recover it.

## Patch 7 addendum (late 2026-10-19): adaptive trial defocus

Patch 7 makes the Refocus test retry a **no-trigger** trial with the defocus doubled (up to 2 retries, cap
2000 pulses). This is aimed at the 50 mm + speedbooster session: there `+-60` gave only a 4–6 % sharpness
drop, so two trials never triggered; `+-150`/`+-400` worked. Expected new lines in AFRTEST.LOG:

```
# RT_RETRY def0=60 def=120 attempt=2
# RT_TRIAL def=120 def0=60 attempt=2 ... runs=1 ... ok=1 ...
# RTEST false_trig=... ok=.../5 ... notrig=0 valid=5 false_pass=0 retries=1
```

At 18 mm the retries should stay `retries=0` and the previous `ok 5/5, false_trig 0` result must be
unchanged. A trial that fails at 60, 120 and 240 is genuinely invalid (`no_trigger=1`) — the retry only
addresses a too-small stimulus, not a texture-less subject. Also record `mm_pre=`/`mm_post=` from the
AFDRIVE header/epilogue when you change the zoom, and set Fine step manually (see the freeze box above).

## Pass/fail summary for this retest

| Check | Pass |
|---|---|
| 18 mm Refocus | `notrig=0 valid=5 false_pass=0 ok≥4/5 false_trig=0` |
| 55 mm Refocus | honest (no false pass): `verify local better` or stable reference and `ok≥4/5`; a soft-ref report is acceptable evidence, a 5/5 against a 34k reference is not |
| 50 mm defocus retry | `# RT_RETRY def0=60 def=120` appears, the retried trial triggers, `retries≥1`; 18 mm stays `retries=0` |
| focal freeze | `mm_pre`/`mm_post` equal after a zoom with the monitor open; after close-monitor → zoom → AF drive the header shows the new mm (EXIF matches) |
| auto refocus on blur | attempts on most blur episodes; `# aref abs re-arm` appears after failures; no repeated fail-loop hunting |
| safety | no crash; Movie Servo off; monitor opens once |
