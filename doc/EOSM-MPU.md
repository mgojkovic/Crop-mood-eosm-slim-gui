# EOS M MPU investigation: protocol, tools and findings

Camera: Canon EOS M, firmware 2.0.2, EF-M 18-55 STM.
Branch: `eosm-slim-gui`.
Period: October 2026.

This document records what was learned about the EOS M MPU, the communication
channels that ML has not used yet, and the temporary `mpu_lab` module that was
built to dump the MPU and send raw messages. It complements
[`EOSM-AF.md`](EOSM-AF.md), which covers the Auto refocus work and the earlier
(mostly negative) attempts to drive the lens from the ICU.

The short conclusion: the MPU controls the lens and the AF engine, the EOS M
MPU is a separate Renesas F74964A with its own firmware and EEPROM, and there
are three communication layers on top of the one ML has been using. The most
promising next step is to dump the MPU firmware through Canon's own monitor and
disassemble it.

---

## 1. What the MPU is

- On the EOS M the MPU is a separate microcontroller. a1ex identified the parts
  used by the DIGIC 5 era: 650D and EOS M use **F74964A**, 5D3/6D use
  F74965A, and 70D/100D/700D use F74966A, later identified as a Renesas SH7231
  (SH2A core) — [MPU communication, page 4](https://www.magiclantern.fm/forum/index.php?topic=17596.75),
  [page 3](https://www.magiclantern.fm/forum/index.php?topic=17596.50).
  F74964A is presumably a smaller sibling in the same SH2A family.
- The MPU runs its own firmware and has its own EEPROM. Canon's main CPU
  ("ICU", where ML runs) talks to it over SIO3/MREQ using short messages.
- Vanilla `mpu_dump` already produces valid MPU ROM/EEPROM dumps on the 5D3
  (same F749 family) through Canon's `MonRead`:
  [MPU communication, msg199973](https://www.magiclantern.fm/forum/index.php?topic=17596.msg199973).
  The EOS M ICU ROM also contains `MpuMonOpen`, `MpuMonRead`,
  `MpuMonClose` and `MpuMonWrite`, so the same read-only dump path should be
  available on the EOS M.

## 2. Message format and channels

A message is:

```
[transfer_size] [group] [op] [payload...]
```

where `transfer_size = (message_size + 2) & 0xFE` (see a1ex's explanation in
[MPU communication](https://www.magiclantern.fm/forum/index.php?topic=17596.0)).
The ICU packet builder is at ROM `0xFFA6CE54`, RAM `0x38C4`:

```
send(group, op, len, data)   // len is the payload length, max 100
```

There is a second send variant at ROM `0xFFA6CED8`, RAM `0x3948`, used by the
COM_CPU / group 7 channel. The `@MPU` hook in crop_rec currently only covers
`0x38C4`, so group 7 traffic is invisible to the existing logs.

Observed groups:

| Group | Meaning |
|---|---|
| 4 | remote switches / button events, e.g. `(4,0x16,3)` = PROP_REMOTE_SW1 |
| 7 | COM_CPU / bulk data channel; `assistmpu` uses `(7,6,33)` chunks + `(7,8,1)` |
| 8 | MPU monitor / memory access (`MpuMonRead` uses this) |
| 9 | LV property channel (the layer ML has used) |

## 3. Group 9: the property channel

The ICU→MPU property table is at `0xFF902EC8` (80 entries); the MPU→ICU table
is at `0xFF902BD8` (20 entries of 16 bytes). The property→message dispatcher is
`0xFF324950`; a property change is posted through Canon's
`_prop_request_change` at `0xFF11FB04`.

### 3.1 Outgoing properties relevant to lens / AF

These were extracted by emulating the `0xFF324950` dispatcher with the ROM's
own property IDs and checking the resulting `send(group, op, len, data)` call.

| Property | Message | Payload | Notes |
|---|---|---|---|
| `0x80050001` LV_FOCUS | `(9,1,5)` | `[hi, lo, FF, FF, 00]` | Canon's sender is `0xFF184494`, data `[1, hi, lo, FF, FF]` |
| `0x80050003` FOCUS_STOP | `(9,3,1)` | `[00]` | handler always sends |
| `0x80050004` LV_APERTURE | `(9,4,4)` | 3 bytes + 0 | |
| `0x80050009` FOCUS_STATE | `(9,0x0B,2)` | `[state, 00]` | Canon's AF-end message |
| `0x80050015` DISPSIZE | `(9,0x11,2)` | 1 byte + 0 | |
| `0x8005001D` LensSearchDriveStart | `(9,0x13,4)` | `[end, speed, timeout, 00]` | property data is `{gate,b1,b2,b3}`; `gate != 0` is required to send, then `b1..b3` are sent |
| `0x80050020` | `(9,0x15,14)` | 14 bytes | AEWB exposure parameters |
| `0x80050026` LV_FOCUS_DATA | `(9,0x17,33)` | 32 bytes + 0 | per-frame AF data feed |
| `0x8005002A` | `(9,0x1A,5)` | 4 bytes + 0 | |
| `0x80050034` | `(9,0x1F,3)` | 2 bytes + 0 | |
| `0x80050038` | `(9,0x24,5)` | 4 bytes + 0 | |
| `0x80050039` | `(9,0x21,61)` | 60 bytes + 0 | large AF configuration block |
| `0x8005003C` AF session | `(9,0x25,16)` | 15 bytes + 0 | handler copies exactly 15 bytes at `0xFF3256B8` |
| `0x80050043` | `(9,0x2C,2)` | 1 byte + 0 | |
| `0x80050047` | `(9,0x2F,2)` | 1 byte + 0 | |

Corrections to the earlier AF notes:

- `0x80050027` (FOCUS_CMD) is **not** in the outgoing table. It is only an
  MPU→ICU property (`(9,0x18)`), so sending it through the ICU property layer
  can never produce an MPU message.
- `0x80050028` is likewise MPU→ICU only (`(9,0x19)`).
- `0x8005001D` is a real outgoing “lens search drive start” command, but no
  Canon EOS M code sends it. It is implemented in the MPU handler with a
  leading enable byte.

### 3.2 Full ICU→MPU table (ROM `0xFF902EC8`, 80 entries)

```
 0 80030000  1 80010000  2 80020002  3 80020004  4 80020005  5 80020003
 6 80020007  7 80020008  8 80030005  9 80030006 10 0201000d 11 0201000e
12 80030007 13 80030008 14 8003000e 15 02010000 16 02010001 17 02010002
18 02010003 19 02010004 20 02010005 21 8003000f 22 80030015 23 8002000a
24 80000000 25 0206000a 26 0206000b 27 0206000c 28 80030019 29 8003001a
30 80050001 31 80050003 32 80050004 33 80050009 34 80050015 35 8004000a
36 80030020 37 80040010 38 8005001d 39 80050020 40 80050026 41 80070000
42 80000039 43 8003003d 44 8005002a 45 0205000d 46 8004001f 47 80030040
48 80050034 49 80000043 50 8003004e 51 80030048 52 8003004b 53 80030051
54 80050038 55 80050039 56 80080000 57 8002001f 58 80070003 59 80080006
60 80080007 61 80080008 62 8008000a 63 8008000b 64 8008000c 65 8005003c
66 80020023 67 80050043 68 80020028 69 8003006a 70 8003006b 71 8008000f
72 80080010 73 80080012 74 80080014 75 80080016 76 8003006e 77 80050047
78 80080017 79 80080018
```

### 3.3 MPU→ICU table (ROM `0xFF902BD8`, 20 entries)

`(group, op, property, 0)`:

```
(9,0x1c,80050031) (9,0x1d,80050032) (9,0x2d,80050044) (9,0x30,80050048)
(9,0x00,80050000) (9,0x02,80050002) (9,0x05,80050005) (9,0x0c,8005000a)
(9,0x0d,8005000b) (9,0x0e,8005000c) (9,0x0f,8005000d) (9,0x0a,80050008)
(9,0x10,80050010) (9,0x12,8005001b) (9,0x14,8005001e) (9,0x18,80050027)
(9,0x0b,80050029) (9,0x19,80050028) (9,0x1b,8005002b) (9,0x1e,80050033)
```

`(9,0x14)` is especially interesting: leegong reports it carries a short
`LensFocusPosition` plus a long timing value — the focus-position feedback that
is missing on the EOS M — [
MPU communication, page 2](https://www.magiclantern.fm/forum/index.php?topic=17596.25).

## 4. The missing lens commands

### 4.1 `(9,0x13,4)` LensSearchDriveStart

The ICU property handler is `0xFF325368`:

- property data is 4 bytes: `{gate, b1, b2, b3}`
- if `gate == 0`, nothing is sent
- otherwise the MPU receives `(9,0x13,4)` with payload `{b1, b2, b3, 0}`

Leegong's reverse engineering of the same message on the 550D MPU gives:

```
char msg[] = { 0x06, 0x09, 0x13, ..., end, AF_driving_speed, unknown_param };
// focus to infinite or nearest end; last byte probably a timeout
```

[MPU communication, page 1](https://www.magiclantern.fm/forum/index.php?topic=17596.0).
This is the closest thing to a “move the lens” command that exists in the MPU
protocol and no EOS M firmware path sends it. It has not been tried on the
EOS M.

### 4.2 `(9,0x01,5)` relative focus drive

a1ex's working 5D3 message is:

```
08 09 01 num_steps_hi num_steps_lo 07 00 00
```

i.e. `(9,1,5)` with `[hi, lo, 0x07, 0x00, 0x00]`. The `0x07` is the focus
driving speed (leegong, later in the same thread). ML's property path always
produces `[hi, lo, FF, FF, 00]`; on the EOS M that difference has never been
tested. The `MPU lab` module can send either form.

### 4.3 `(9,0x25,16)` AF session

The ICU handler copies exactly 15 bytes and appends `0`, so the request is a
15-byte property value and the MPU message is 16 bytes. Canon EOS M sends this
message during a half-press, with a first byte of `0x02`; QEMU's EOS M spell
data contains both `[02 00 ...]` and all-zero versions
([`EOSM.h`](../contrib/qemu/eos/mpu_spells/EOSM.h)).

### 4.4 `(9,0x21,61)` AF configuration

The handler copies 60 bytes and sends 61. This is a large per-AF-engine
configuration block. Together with `(9,0x17,33)` (the per-frame AF data feed),
this is likely where the AF area / search configuration lives. Replaying the
x1 block at x5, or the x5 block at x1, is a possible way to see which fields
control the magnified-view behaviour.

## 5. The monitor / debug channels

### 5.1 MPU monitor (read-only path)

The EOS M ICU ROM has Canon's MPU monitor:

| Function | ROM address |
|---|---|
| `MpuMonOpen` | `0xFF1DD098` |
| `MpuMonClose` | `0xFF1DD118` |
| `MpuMonRead` | `0xFF1DD2B8` (kind, addr, size, buffer) |
| `MpuMonWrite` | around `0xFF1DD474` (writes; **not** used by this probe) |

`MpuMonRead` is the function behind `call("MonRead", ...)` in
[`modules/mpu_dump`](../modules/mpu_dump/mpu_dump.c). Vanilla `mpu_dump`
works on the 5D3, whose MPU is the same F749 family, so a read-only EOS M dump
is expected to work. `mpu_lab` calls the ROM functions directly so it does not
depend on `MonRead` being present in the ML symbol file.

### 5.2 `Msg 08 02` — factory/debug command table

Leegong decoded a large command table in the 550D MPU firmware for message
`08 02` ([MPU communication, page 3](https://www.magiclantern.fm/forum/index.php?topic=17596.50)):

- `fun0F_custom_LensCMD`
- `fun10_FocusScan`
- `fun19_Lens_RTX_1byte`
- `fun49_read_EEPROM`
- `fun6A_write_EEPROM`
- `fun60_cal_MPU_CodeArea_checksum`
- `fun75_read_RAM_byte`
- `fun03_load_EEPROM_2_RAM`
- `fun48_copy_Lens_aperture_data`

Leegong's comment: “these Msg08 02 might belong to factory debug section, they
don't belong to normal Msg workflow”. The EOS M ICU never sends `(8,2)` except
through the monitor. If the F74964A accepts these commands on the normal
channel, this is a direct route to the lens and EEPROM. **Do not try the write
commands until the MPU ROM has been dumped and the argument formats are known.**

### 5.3 Group 7 / COM_CPU and `assistmpu`

The ICU has an `assistmpu` command implementation around `0xFF1447C0` that
uploads data in 32-byte chunks with `(7,6,33)` and finishes with `(7,8,1)`.
The second send variant (`0x3948`) implements a COM_CPU state machine with
`COM_CPU_STATE`, `COM_CPU_SEND_TO_MPU`, `COM_CPU_ACCEPT_START/END` and
`COM_CPU_RESULT` messages. This is a separate bulk/command channel and is not
covered by the existing `@MPU` hook.

### 5.4 Hidden AF debug info

Leegong found that `Msg 05 08` (AF debug information) is disabled by a 16-bit
setting in the MPU EEPROM at `0x1042`; enabling it changes what the MPU sends
in `Msg 05 03` / `Msg 05 09` (AF debug info). This could expose raw AF internals
and focus data without any lens commands. It requires EEPROM access, which is
why the MPU ROM/EEPROM dump comes first.

## 6. What was built and flashed

### 6.1 `modules/mpu_lab`

New temporary module: [`modules/mpu_lab`](../modules/mpu_lab). It is in the slim
module list in [`modules/Makefile.modules.user`](../modules/Makefile.modules.user).

Menu: **Focus → MPU lab** (the slim GUI's Focus tile is short and reachable;
Settings was not). Entries:

1. **Dump memory** — `ROM 1 MB`, `EEPROM 64 KB`, `RAM 24 KB`, `test 4 KB`
2. **Run dump** — read-only, starts after 2 s, writes `ML/LOGS/MPU-*.BIN`.
   Checks `MpuMonOpen` and every 1 KB `MpuMonRead` return value (non-zero =
   error: stop, show code and offset), always calls `MpuMonClose`, sleeps 10 ms
   per chunk, and logs the result (see below). The screen says if the dump is
   all 00 or all FF.
3. **MPU command** — raw one-shot commands:
   - `(9,0x13,4)` search-drive variants (`[0/1, end, speed, 0]`)
   - `(9,0x01,5)` focus drive with speed byte `0x07`, ±1/15/50
   - `(9,0x25,16)` AF session `02/00/01`
   - `(9,0x03,1)` focus stop, `(9,0x0B,2)` focus state
4. **Run command** — sends the selected raw message after 1.5 s; refused while
   recording; a stop is sent after the drive/search commands
5. **MPU sequence** / **Run sequence** — multi-step tests (table
   `mpu_seqs` in `mpu_lab.c`, easy to extend):

   | Sequence | Steps (then always: `(9,3,1)` stop, and session close if opened) |
   |---|---|
   | session + drive spd7 | `(9,0x25,16)` `[02,0…]` → 200 ms → `(9,1,5)` `[00 32 07 00 00]` → 800 ms → `[FF CE 07 00 00]` → 800 ms |
   | session + drive ML form | same with `[00 32 FF FF 00]` / `[FF CE FF FF 00]` |
   | search near/far | `(9,0x13,4)` `[00 01 07 00]` → 1.5 s → stop → `[00 00 07 00]` → 1.5 s → stop |
   | session + search | open session, the search pair, stop |

   Session close is `(9,0x25,16)` with an all-zero payload. Sequences need LV,
   wait up to 7 s for you to close the menu, and abort between steps if
   recording starts, LV is lost or the menu is opened; the final stop/close is
   still sent.

**Reply capture.** `mpu_lab` has its own `PROP_HANDLER`s for the MPU→ICU
properties `0x80050000` LV_LENS, `0x80050002` LV_FOCUS_DONE, `0x8005001B`,
`0x8005001E` (focus pulse count), `0x80050027`, `0x80050028`, `0x80050029`
LV_AF_RESULT and `0x80050045` focal distance. From the send until the end of
the step (2 s for single commands; the step's wait time in a sequence) each
handler counts arrivals and keeps the first 8 bytes of the last value. Handlers
never touch the file; the command task writes the log afterwards and shows a
NotifyBox such as `done:3 fpc:0 cmd:1 dist 98->85`.

**`ML/LOGS/MPULAB.LOG`** (appended; timestamps are `get_ms_clock()`):

```
<ms> CMD name=<cmd> seq=<seq|-> step=<n> sent=[GG.OO.LL:hex bytes | ...] ret=<r1,r2> dist=<before>-><after> win=<ms>ms
<ms> REPLY name=<cmd> prop=<hex> tag=<tag> n=<arrivals> len=<last length> last=[first 8 bytes hex]
        (one REPLY line per watched property, n=0 when nothing arrived)
<ms> DUMP file=<f> kind=<k> addr=<hex> req=<n> got=<n> zero=<n> ff=<n> sum=<hex32> empty=<YES|no> result=<ok|ERROR> err=<code>@<offset> ms=<t>
```

The module uses Canon's MPU monitor addresses directly and the ICU packet
builder at `0x38C4`. It does **not** write EEPROM or MPU memory.

### 6.2 Build

Final build command:

```
make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3
```

The resulting zip is `platform/EOSM.202/magiclantern-crop_mood.2026Oct18.EOSM202.zip`
and contains `ML/modules/mpu_lab.mo`.

Build gotchas met:

- ML only auto-loads **core** modules. A non-core module needs an empty
  `ML/SETTINGS/<name>.en` flag. Created `mpu_lab.en` on the card. Toggling the
  module OFF in the **Modules** menu removes that flag; re-enable it there or
  recreate the empty file.
- `modules/readme2modulestrings.py` is Python 2, and the module Makefile uses
  `PYTHON=python2`; on this machine that fails. Keeping a valid
  `modules/mpu_lab/module_strings.h` (newer than `README.rst`) avoids the
  generator. After `make clean`, recreate the empty `module_strings.h`
  placeholders before building.
- `is_camera("EOSM","2.0.2")` is not reliable in a module `init` at boot:
  `firmware_version` may not be populated yet. The first `mpu_lab` build
  returned early and never added the menu. Fixed by making `menu_add(...)`
  unconditional in init; the tasks still check the camera at runtime.
- Install `autoexec.bin`, `ML/modules/EOSM_202.sym` and all `.mo` from the same
  build.

### 6.3 Flashed card

- Card: `/Volumes/EOS_DIGITAL`
- Previous card state backed up to `/tmp/mpu_lab_backup_20261018_203849/`
- `mpu_lab.mo` on the card (fixed unconditional-menu build) matches
  `modules/mpu_lab/mpu_lab.mo`, MD5 `185adee4a70007ce3a82c5a4baa10cbc`.
- `ML/SETTINGS/mpu_lab.en` recreated after it was found missing during the
  second on-camera check (the Modules menu toggle removes it).
- Card was synced and unmounted after flashing.

The final full build (21:16) contains the same `mpu_lab.mo` MD5 and the card was
updated with that build. The enable flag was restored at 21:19. Menu visibility
after the unconditional-`menu_add` fix was not yet confirmed on the camera at
the time of writing. If the module loads but the entry is still absent, the
fallback is to register it from a delayed task after the menu subsystem is up,
or to check the **Modules** menu for a link error.

## 7. How to use the probe

1. Boot the camera with the card.
2. **Settings → AF diag log → ON + MPU** (captures raw `@MPU` sends in
   `ML/LOGS/AFDIAG.LOG`).
3. **Focus → MPU lab**.
4. First run **Dump memory → test 4 KB → Run dump**. This proves the monitor
   works before a long read.
5. Then **ROM 1 MB → Run dump**; optionally EEPROM and RAM.
6. Copy `ML/LOGS/MPU-*.BIN`, `ML/LOGS/MPULAB.LOG` and `ML/LOGS/AFDIAG.LOG` off the card.

For a raw test, start with **`search end1 spd7`** or **`focus +15 spd7`**,
one command per run, and watch the lens / `focus_dist`. The `session` commands
should wait until the MPU ROM has been inspected, or be run via the
**MPU sequence** entries (which close the session afterwards).

## 8. Next steps (in order)

1. **Dump the EOS M MPU ROM** with `mpu_lab` (read-only). Confirm the dump
   looks like code (not all `FF`/`00`).
2. **Disassemble it as SH2A.** Identify the group/op dispatch, the lens
   command builder, the `08 02` table, the AF state machine and the EEPROM
   layout.
3. From that, recover the exact argument formats for:
   - `(9,0x13,4)` search drive (end, speed, timeout)
   - `(9,0x01,5)` speed/step semantics
   - `(9,0x25,16)` AF session
   - `(9,0x21,61)` AF configuration block
   - `(8,2,...)` custom LensCMD / FocusScan / Lens RTX
4. Test a direct lens search/drive at x5 and see whether the MPU sends
   `(9,0x14)` focus position back.
5. Only then consider EEPROM writes (hidden AF debug info at `0x1042`) or a
   custom MPU task/firmware.

The larger goal — ML-driven or continuous AF under a crop preset — can take two
routes:

- **MPU route:** use `(9,0x13)` / `(9,0x01)` plus `(9,0x14)` feedback for a
  closed-loop AF that does not depend on Canon's x5 AF area.
- **ICU route:** patch the ICU servo gate (`+0x710`, `+0x184`) so Canon’s own
  servo can run at x5. This is outside the MPU but may be simpler once the
  MPU behaviour is understood.

## 9. Safety and rollback

- The dumps are read-only.
- Raw messages are one-shot, delayed and refused while recording. The module
  always sends `(9,3,1)` after the drive commands.
- The module does **not** implement EEPROM writes, `MEMW`, or `(8,2)` write
  commands.
- If the camera hangs, pull the battery. The module has no persistent settings
  and is OFF after restart.
- Restore the backup in `/tmp/mpu_lab_backup_20261018_203849/`
  (`autoexec.bin`, `ML/modules/EOSM_202.sym`, `ML/modules/*.mo`) to roll back.

## 10. References

### ML forum

- [MPU communication](https://www.magiclantern.fm/forum/index.php?topic=17596.0),
  [page 2](https://www.magiclantern.fm/forum/index.php?topic=17596.25),
  [page 3](https://www.magiclantern.fm/forum/index.php?topic=17596.50),
  [page 4](https://www.magiclantern.fm/forum/index.php?topic=17596.75)
- [EOSM: is there hope?](https://www.magiclantern.fm/forum/index.php?topic=17573.0)
- [Programmatic focus control and absolute/relative focus position](https://www.magiclantern.fm/forum/index.php?topic=4997.0)
- [EF Lens communication protocol](https://www.magiclantern.fm/forum/index.php?topic=18308.0)
- [DIGIC 8+ MPU investigation](https://www.magiclantern.fm/forum/index.php?topic=25661.0)
- [Magic Lantern wiki: Tx19a](https://magiclantern.wikia.com/wiki/Tx19a)

### Repo

- [`modules/mpu_dump`](../modules/mpu_dump/mpu_dump.c) — the original read-only
  MPU dump module.
- [`contrib/qemu/eos/mpu_spells/EOSM.h`](../contrib/qemu/eos/mpu_spells/EOSM.h)
  and [`known_spells.h`](../contrib/qemu/eos/mpu_spells/known_spells.h) —
  captured EOS M MPU message sequences.
- [`modules/crop_rec/crop_rec.c`](../modules/crop_rec/crop_rec.c) — the AF
  diagnostics and the “AF lens test MPU” series that preceded this probe.
- [`doc/EOSM-AF.md`](EOSM-AF.md) — Auto refocus and the earlier lens-drive
  attempts.

### Local analysis scratchpad

The ROM1 dump and the Python/Unicorn scripts used to extract the tables above
live outside the repo in the session scratchpad
(`/private/tmp/claude-501/-Users-mgojkovic-repos-Crop-mood-eosm-slim-gui/<session>/scratchpad`).
The key scripts were `map_props.py`, `re/dis.sh`, `re/refs.py` and
`re/callers.py`.
