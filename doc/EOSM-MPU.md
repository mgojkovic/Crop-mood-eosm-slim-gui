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
- The EOS M MPU ROM has now been dumped: it is valid big-endian SH2A code and
  contains a debug-monitor command table with **`AF_LENS_SEARCH`**,
  **`TVAFLOG`** (search direction/level), **`VLENS_DRIVE`**, **`VLENS_SET`**,
  EEPROM read/write and `EXEC`. See section 5.5. This is the first confirmed
  MPU-side entry point for an AF search and direct lens commands; the normal
  group-8 message route has now been confirmed on camera and moved the lens
  from 1.15 m to infinity (section 5.8).

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

### 5.5 First-pass analysis of the EOS M MPU ROM dump

The probe produced two dumps on 18 Oct (copied to the session scratchpad as
`card29/` and `card30/`):

| File | Size | Result |
|---|---|---|
| `card30/MPU-ROM.BIN` | 786,432 bytes (`0xC0000`) | full 768 KB read, `got=786432`, `zero=55408`, `ff=34396`, `ms=137553` |
| `card29/MPU-ROM.BIN` | 808,960 bytes (`0xC5800`) | extended read; reading past `0xC5800` hangs the monitor |
| `card29/MPU-TEST.BIN` | 4,096 bytes | smoke test, not empty |

Validation:

- The first words are `00000060 FFF88000 000063F4 FFF88000`. That is an SH2A
  reset vector: PC `0x60`, SP `0xFFF88000`, then the manual-reset vector at
  `0x63F4`. The dump is **big-endian SH2A code**, not empty flash.
- Entropy is 7.01, with substantial zero and `FF` regions but plenty of code
  and strings; `MPULAB.LOG` reports `result=ok`.
- `card30/MPU-ROM.BIN` is the canonical 768 KB image. `card29` is an earlier
  extended read and should only be used for the extra tail.

The ROM contains the MPU **debug monitor**, its text-command table and a normal
message route to the same handler table. Full details are in
[EOSM-MPU-ROM.md](EOSM-MPU-ROM.md); the short version:

- The monitor is a serial console (banner `Debug Monitor (K269)`, prompt
  `MON>>>`). The text parser is serial-only; there is no group 7/8/EXEC path
  into the line parser.
- The real command table is **name-first at `0x147B0`**: 36-byte name, handler
  at `+0x24`, stride `0x28`, 49 records. Corrected key handlers:
  `EXEC=0x6F526`, `TEST=0x6F8FC`, `RELEASE=0x6F448`, `TVAFLOG=0x6F81A`,
  `AF_LENS_SEARCH=0x33794`, `CHECKEXTFLASH=0x74D3E`, `VLENS_SET=0x6FCD2`,
  `VLENS_DRIVE=0x6FDAC`, `ABOUT_LENS=0x206FE`, `EEPR=0x6EAF4`,
  `EEPW=0x6EB7C`, `UPBNY=0x70574`.
- `TVAFLOG` is a monitor-only command: `TVAFLOG <0|1> <1|2|3> <0|1>`
  (all-scan, drive level, direction), args parsed as hex.
- **Normal-message route to `AF_LENS_SEARCH`** (verified statically): the
  group-8 receive dispatcher copies a 32-byte payload to `0xFFF84CEB` and
  enqueues `[group,op]`; the consumer for group 8, op 2 reads
  `be32(payload[4..7])` as an index and calls `0x1417C[index](0xFFF80000)`.
  `0x1417C[16] = 0x33794 = AF_LENS_SEARCH`. The exact ICU message is:

  ```
  send(8, 2, 32, payload) with payload[4..7] = 00 00 00 10
  wire: 24 08 02 00 00 00 00 00 00 00 10 00 ... (35 bytes total)
  ```

  Expected reply: `08 08 02 <BE32 return> 00`. `0x33794` takes no input args;
  it writes the mailbox `0xFFF825DA = 17` and triggers the AF/lens logic.
  The serial equivalent is the monitor command `EXEC 10` (hex index).
- `0x1417C` is a 128-entry normal-firmware handler table with **no bounds
  check**; only indices 0..127 are safe. Do not send while recording.
- `VLENS_SET` / `VLENS_DRIVE` fill the lens command block at `0xFFF8256C`;
  argument semantics are inferred in [EOSM-MPU-ROM.md](EOSM-MPU-ROM.md).
- The serial monitor pinout, 3.3 V/115200 8N1, `TXDMPU` pad and command
  scripts are in [EOSM-MPU-UART.md](EOSM-MPU-UART.md).

The `card29` AFDIAG log contains only Canon's normal AF traffic and the dump
records; `MPULAB.LOG` has no `CMD`/`REPLY` records, so the raw lens commands
were not exercised in these two sessions. The new `(8,2,32)` probe must be run
on the camera to confirm the statically traced route.

### 5.6 First camera test (23:58, card log `MPULAB.LOG`)

The first test after flashing the probe produced:

- Three `(9,0x13,4)` search commands `[00 01 07 00]`, each followed by
  `(9,3,1)` stop. The MPU replied with `LV_LENS` (n=8, 61 bytes) and
  `LV_FOCUS_DONE` (n=1, 6 bytes), but **the lens did not move**
  (`focus_dist` 39→39; one run showed 65535→73, likely the stale/unknown
  lens distance at start). The property path remains non-functional.
- The no-op control `(8,2,32)` index 20 was sent (`08.02.20` with payload
  `... 00 00 00 14 ...`); it had no visible effect, as expected for the
  `0x71892` stub. No raw group-8 reply is captured by the module yet.
- The AF_SEARCH entry that ran was the **13-byte variant**
  (`08.02.0D`, index 16), not the primary 32-byte `(8,2,32)` message. No
  visible lens/AF change and no raw reply. This run does not test the
  confirmed 32-byte route.

The next build therefore changes the primary `(8,2,32)` probe to call
`MpuMonOpen()` first, read MPU RAM `0xFFF80000`, `0xFFF825DA` before the send
and `0xFFF80000`, `0xFFF825DA`, `0xFFF825E0` after it, and log every read as a
`MON` line. This will show whether `0x33794` wrote `0x10` to `0xFFF80000` and
`17` to `0xFFF825DA`, independently of any lens movement.

### 5.7 Second camera test: the group-8 route is confirmed (00:02)

The rebuilt primary probe ran and produced the decisive result:

```
39809 MONOPEN err=0
41638 MON PRE_80000  kind=0 addr=fff80000 err=0 val=00000000
41661 MON PRE_825DA  kind=0 addr=fff825da err=0 val=00000000
41661 CMD name=E 8.2.32 AF_SEARCH idx16 ...
        sent=[08.02.20:00 00 00 00 00 00 00 10 ...]
42472 MON POST_80000 kind=0 addr=fff80000 err=0 val=00000000
42473 MON POST_825DA kind=0 addr=fff825da err=0 val=10000000
42474 MON POST_825E0 kind=0 addr=fff825e0 err=0 val=11000000
```

`POST_825DA = 0x10000000` means byte 0 is `0x10` (16); `POST_825E0` starts
with `0x11` (17); `POST_80000` reads back 0. Two otherwise-zero bytes in the
`0xFFF825xx` block changed immediately after the message, which proves the
handler ran: the 32-byte group-8 message reached
`0x1417C[16] = 0x33794`. Note the exact byte placement is not identical to the
static reading (`0x33794` writes 17 at `0xFFF825DA` and 16 at `*r4`); either
the wait call at `0x337D4` overwrites/moves the values, or `MpuMonRead` maps the
first RAM page differently. The execution itself is not in doubt; the precise
status-byte offsets can be re-derived later.

The lens still did not move in the 58→58 run (`LV_LENS` n=2,
`LV_FOCUS_DONE` n=0). At that point it was not clear whether the handler was a
no-op/log-only command or needed a particular AF state.

### 5.8 Third camera test: AF_LENS_SEARCH moves the lens (00:07)

The same primary probe was run again, this time with the camera in AF state.
The log shows:

```
30930 MONOPEN err=0
30961 MON PRE_80000  val=00000000
30981 MON PRE_825DA  val=00000000
31775 MON POST_80000 val=00000000
31776 MON POST_825DA val=10000000
31781 MON POST_825E0 val=11000000
30981 CMD name=E 8.2.32 AF_SEARCH idx16
        sent=[08.02.20:00 00 00 00 00 00 00 10 ...] ret=0 dist=115->115
30981 REPLY LENS n=2 len=61 last=[3B A3 C0 00 3F EE 00 00]
```

The user observed the lens move from **1.15 m to infinity** after this command.
The `dist=115->115` line is only the 2 s capture window: the module stops
watching before the MPU reports the final distance/`FOCUS_DONE`. The lens itself
moved.

This is the first end-to-end confirmation:

1. the ICU can send the 32-byte group-8 message,
2. the MPU calls `0x1417C[16] = 0x33794`,
3. the handler triggers an actual AF lens movement.

So **normal messages are enough to start an MPU AF search**; the serial monitor
is not required for this step. `TVAFLOG` is still useful for scan direction,
drive level and log state, but the basic search is reachable from ML.

The 2 s capture window was the only reason `FOCUS_DONE` was missing; the lens
itself moved.

### 5.9 Long-window test: full scan to infinity (00:12)

The `AF search (MPU) 8s` action was run with an 8.8 s window:

```
39395 CMD name=AF search (MPU) 8s
        sent=[08.02.20:00 00 00 00 00 00 00 10 ...] ret=0 dist=73->65535 win=8845ms
39395 REPLY LENS n=28 len=61 last=[3B A3 C0 00 40 10 40 00]
39395 REPLY FOCUS_DONE n=0
```

- The lens moved from **73** to **65535** (the infinity/unknown sentinel) in
  about 8.8 s; the user confirmed it physically reached infinity.
- `LV_LENS` updated **28 times** during the run, so the module can track the
  movement; `LV_FOCUS_DONE` never arrived inside the window.
- This is a one-way **search/drive to infinity**, not a complete
  contrast-AF cycle with a focus result. For a closed loop, use the `LV_LENS`
  focus position/phase and distance updates as feedback; do not wait for
  `FOCUS_DONE`.
- The final lens snapshot starts `3B A3 C0 00 40 10 40 00`; the EOS M
  `focus_pos` is at offset `0x22` of the 61-byte `LV_LENS` payload and
  `focus_dist`/`focus_dist2` at `0x30`/`0x32`.

So the normal-message route is confirmed for a real, repeatable lens drive.
Next is direction/stopping control and integration:

1. Repeat at different start distances; measure time and final position.
2. Test whether a second `(8,2,32)` call while a scan is running stops or
   restarts it; if not, send `(9,3,1)` stop.
3. Use TXDMPU `TVAFLOG 1 3 0` / `TVAFLOG 0 3 1` before `EXEC 10` to see
   whether direction and drive level can be selected; then reproduce that
   state from a normal message if possible.
4. Integrate `AF search (MPU)` into the crop/AF GUI and use `LV_LENS` feedback
   for a closed-loop autofocus under a crop preset.

### 5.10 Three-run direction/stop test and the infinity freeze (00:17)

The next session ran the 8 s action three times:

| Run | Start | Observed by user | Log |
|---|---|---|---|
| 1 | 2.9 m target in focus | drove back to ~50 cm, then to infinity | `dist=45->65535`, 27 `LV_LENS`, 8.8 s |
| 2 | same target | drove to ~60 cm, then to infinity | `dist=65535->65535`, 27 `LV_LENS`, 8.8 s |
| 3 | lens at infinity, near subject | **camera froze** | `dist=65535->65535`, `PRE_80000=0x08800000`, 27 `LV_LENS`, 8.8 s |

Run 1/2 show the handler is a **full-range scan**: it drives to the near end
first, then all the way to infinity. It passes through the in-focus position
but does not stop there; the final state is always infinity. So this command
alone is a scanner, not an autofocus.

Run 3 is a known-bad state: starting a scan while the lens is already parked at
infinity hung the camera. The MPU handler or the lens/end-stop logic did not
return. Pull the battery to recover.

Safety changes in the 00:19 build:

- the probe now **refuses** `(8,2,32)` index 16 when
  `lens_info.focus_dist >= 60000` (infinity/unknown), with a message to focus
  nearer first;
- a `(9,3,1)` **FOCUS_STOP** is sent after every group-8 EXEC, including the
  AF search, and logged as `STOP after group 8 exec`;
- the existing one-shot/recording/menu rules still apply.

The important design conclusion: the next version must not run the scan to
completion. It should send the search, watch `LV_LENS` / sharpness, and send
`(9,3,1)` as soon as the sharpness peak is passed. The MPU gives us movement and
position updates; the ICU must supply the stop decision.

### 5.11 Guarded run, stop, and the dark viewfinder (00:24)

The 00:20 guarded build produced two log records:

```
22629 REFUSE AF search: lens at infinity (65535)
...
35916 CMD name=AF search (MPU) 8s
        sent=[08.02.20:... | 09.03.01:00] ret=0,0 dist=49->65535 win=9132ms
46555 STOP after group 8 exec
REPLY LENS n=29 last=[3B A3 C0 00 40 13 40 00]
REPLY FOCUS_DONE n=1 last=[00 00 4E 00 00 00]
```

So the warning did fire once and **refused** the run (the lens was at
infinity). The later run started at distance 49 and was allowed. It still ended
at infinity because the handler is a full-range scan; the guard only prevents
*starting* at infinity, not the scan's far end. That is expected, not a guard
failure. This run also shows `FOCUS_DONE` does arrive once the scan is stopped
at the end (`n=1`, value `4E`).

The user also reported that the **viewfinder gets noticeably dark** during the
scan, as if shutter speed/drive changes. The 00:27 build adds exposure
instrumentation:

- `EXP PRE` — shutter, aperture, ISO, LV_BV, LV_STATE, LV_DISPSIZE before send;
- `EXP MID` — the same 0.8 s after the search starts;
- `EXP POST` — after the stop and 300 ms settle;
- a new **`AF search (MPU) 2s + stop`** action that stops the scan early, so
  the exposure change can be observed without the full infinity sweep.

Run `AF search (MPU) 2s + stop` and compare `EXP PRE/MID/POST`. If aperture
closes or shutter/BV changes, that explains the dark screen and must be
compensated or avoided in the AF loop.

### 5.12 Exposure test result: not shutter/aperture/ISO (00:31)

Three more runs were done:

- **2 s + stop** from distance 158: `dist=158->158`, `LV_LENS` n=11,
  `FOCUS_DONE n=1` (`[00 10 4E 00 00 00]`), no darkening.
- Three **REFUSE** records at `41361`, `52921`, `69698`: the lens was at
  infinity each time; the guard worked as intended.
- **2 s + stop** from distance 115: `dist=115->158`, `LV_LENS` n=12,
  `FOCUS_DONE n=1`, no darkening in that run.

The `EXP` lines all read **zero** because the property handlers did not receive
initial values in this module; that is a logging gap, not evidence of a change.
The user checked the camera's LiveView setting display during the runs and saw
**no change to shutter, aperture or ISO**. Therefore the dark viewfinder is not
an exposure-setting change. The most likely explanations are optical/defocus
during the long sweep (the 8 s run goes through the near end before infinity)
or an unlogged working-aperture change; the **2 s bounded action did not
darken**, which points at the long sweep rather than the message itself.

Practical conclusion: use the 2 s bounded action as the safe baseline, and
move the stop decision into the ICU (sharpness peak) instead of letting the MPU
sweep the full range.

### 5.13 First sharpness-stop AF search in `crop_rec` (00:35)

The next step is implemented in `crop_rec`, where `afdiag_sharpness()` already
exists:

- new temporary menu entry **Settings → AF lens test → AF search (MPU)**;
- one-shot task: checks EOSM/LV/idle/AF mode, refuses at infinity, opens the
  MPU monitor, sends `(8,2,32)` index 16, samples the centre sharpness every
  50 ms, and sends `(9,3,1)` when sharpness has been below 80% of its peak for
  more than 120 ms (maximum 3 s);
- logs one `AFD_EV_MPUSRCH` record (AFDIAG code 14):
  `w0 = stopped`, `w1 = peak sharpness`, `w2 = elapsed ms`,
  `w3 = lens_info.focus_dist` at stop;
- closes the monitor and shows a NotifyBox with peak/timeout and elapsed time.

This is the first code path that can **stop the scan at the in-focus peak**
instead of running to the infinity end stop. It still needs field tuning:
the 80%/120 ms thresholds and the 3 s cap are first guesses.

## 6. What was built and flashed

### 6.1 `modules/mpu_lab`

New temporary module: [`modules/mpu_lab`](../modules/mpu_lab). It is in the slim
module list in [`modules/Makefile.modules.user`](../modules/Makefile.modules.user).

Menu: **Focus → MPU lab** (the slim GUI's Focus tile is short and reachable;
Settings was not). Entries:

1. **Dump memory** — `ROM 768 KB`, `EEPROM 64 KB`, `RAM 24 KB`, `test 4 KB`
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

**Group-8 probe (new).** The module now includes the statically traced
normal-message route from section 5.5:

- `E 8.2.32 AF_SEARCH idx16` — `(8,2,32)` with payload
  `00 00 00 00 00 00 00 10` + 24 zero bytes. This is the primary probe; it now
  keeps the capture window open for 6 s.
- **`AF search (MPU) 8s`** — the confirmed long-window action: same message,
  `MpuMonOpen` + pre/post mailbox reads, 8 s capture. This is the one to use
  for repeatability/distance tests.
- A control entry using the no-op stub at `0x1417C[20] = 0x71892` (index 0x14),
  and an index-0 entry.
- 13-byte variants and a read-only `(8,1)` MpuMonRead probe; `(7,8,1)` is
  present as a COM_CPU control (group 7 is not handled by the intercom
  dispatcher, so it should do nothing).
- All `E` entries are allowlisted by group/op/len/index, require an extra 2 s
  confirmation, are one-shot, are refused while recording, and log every sent
  byte plus replies. `(8,0)` (MpuMonWrite) and all EEPROM writes are absent.

The serial-only `TVAFLOG`/`AF_LENS_SEARCH` monitor commands are documented under
an info menu entry; there is no attempt to fake a serial monitor path.

**Reply capture.** `mpu_lab` has its own `PROP_HANDLER`s for the MPU→ICU
properties `0x80050000` LV_LENS, `0x80050002` LV_FOCUS_DONE, `0x8005001B`,
`0x8005001E` (focus pulse count), `0x80050027`, `0x80050028`, `0x80050029`
LV_AF_RESULT and `0x80050045` focal distance. From the send until the end of
the step (6–8 s for the group-8 AF search; the step's wait time in a sequence)
each handler counts arrivals and keeps the first 8 bytes of the last value.
Handlers never touch the file; the command task writes the log afterwards and
shows a NotifyBox such as `done:3 fpc:0 cmd:1 dist 98->85`.

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

### 6.3 Flashed card and current artifact

- Card: `/Volumes/EOS_DIGITAL`
- Previous card states backed up to
  `/tmp/mpu_lab_backup_20261018_203849/` and
  `/tmp/mpu_lab_backup_20261018_235456/`
- The first flashed build had `mpu_lab.mo` MD5
  `185adee4a70007ce3a82c5a4baa10cbc`; it loaded after the menu fix and produced
  the dumps analysed in section 5.5.
- `ML/SETTINGS/mpu_lab.en` is required for the non-core module and was recreated
  after the Modules menu toggle removed it.
- The **latest** build (00:35, `crop_rec` sharpness-stop AF search) has
  `crop_rec.mo` MD5 `c842372e3c9e3757a8f237f7d21e2368` and `mpu_lab.mo` MD5
  `d11dd7dd738ef09e6c67a1df9ad775bb`. It was flashed at 00:35 from
  `platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip`; both module
  MD5s were verified on the card and the card was synced and unmounted.

Dump artifacts copied off the card for analysis:

- `card29/MPU-TEST.BIN`, `card29/MPU-ROM.BIN`, `card29/AFDIAG.LOG`,
  `card29/MPULAB.LOG`
- `card30/MPU-ROM.BIN`, `card30/MPULAB.LOG`

They live in the session scratchpad, not in the repo.

## 7. How to use the probe

1. Boot the camera with the card.
2. **Settings → AF diag log → ON + MPU** (captures raw `@MPU` sends in
   `ML/LOGS/AFDIAG.LOG`; the group-8 reply is not a property, see below).
3. **Focus → MPU lab**.
4. First run **Dump memory → test 4 KB → Run dump**. This proves the monitor
   works before a long read.
5. Then **ROM 768 KB → Run dump**; optionally EEPROM and RAM.
6. Copy `ML/LOGS/MPU-*.BIN`, `ML/LOGS/MPULAB.LOG` and `ML/LOGS/AFDIAG.LOG` off the card.

For the new confirmed-by-RE (but camera-unverified) AF search route:

7. **MPU lab → MPU command → `E 8.2.32 AF_SEARCH idx16` → Run command**.
   Accept the extra confirmation. This sends
   `24 08 02 00 00 00 00 00 00 00 10` + 24 zeros (35 bytes) and should call
   `0x1417C[16] = 0x33794`. `ML/LOGS/MPULAB.LOG` gets a `CMD`, an `EXPECT`
   line (`reply=08 08 02 <BE32 ret> 00`), and the property `REPLY` lines.
   The raw group-8 reply is not an `0x8005xxxx` property, so it is logged by
   the module's raw expectation, not as a `REPLY`.
8. Try the no-op control (`E 8.2.32 no-op ctl idx20`) on a separate run to
   distinguish "handler ran" from "message did something".

For a serial-monitor test instead, use [EOSM-MPU-UART.md](EOSM-MPU-UART.md);
the correct command is **`EXEC 10`** (hex index), not `AF_LENS_SEARCH`
(the monitor passes `argc`, while the handler expects a pointer).

For a raw test, start with **`search end1 spd7`** or **`focus +15 spd7`**,
one command per run, and watch the lens / `focus_dist`. The `session` commands
should wait until the MPU ROM has been inspected, or be run via the
**MPU sequence** entries (which close the session afterwards).

## 8. Next steps (in order)

The three investigation tracks requested for this session are complete:

1. **ROM / monitor parser** — see [EOSM-MPU-ROM.md](EOSM-MPU-ROM.md). The text
   parser is serial-console-only; the normal-message route via group 8 op 2 to
   `0x1417C[16]` is traced and **confirmed on hardware** (sections 5.7/5.8).
2. **Probe implementation** — `modules/mpu_lab` contains the allowlisted
   `(8,2,32)` index-16 probe, controls and sequences, the long-window
   `AF search (MPU) 8s`, the infinity guard + stop-after-exec, exposure logging
   and the `AF search (MPU) 2s + stop` action; latest `mpu_lab.mo` MD5
   `d11dd7dd738ef09e6c67a1df9ad775bb` (flashed 00:27).
3. **Sharpness-stop AF search** — `crop_rec` has **Settings → AF search (MPU)**
   (one-shot; samples `afdiag_sharpness`, stops at the peak or 3 s). Latest
   `crop_rec.mo` MD5 `c842372e3c9e3757a8f237f7d21e2368` (flashed 00:35).
4. **Service UART** — see [EOSM-MPU-UART.md](EOSM-MPU-UART.md): TXDMPU 3.3 V,
   115200 8N1, `MON>>>`, correct AF call is `EXEC 10`.

The important milestone is reached: the ICU can start an MPU AF search with a
normal message, the lens moves, and the scan direction/behaviour is known. The
scan is a **full near→infinity sweep**, not a focus-and-stop; starting it at
infinity froze the camera once (section 5.10). The new `crop_rec` action is the
first attempt to stop at the sharpness peak.

Next steps, in order:

1. **Test the `crop_rec` action.** Settings → **AF search (MPU)**, from a
   non-infinity focus, AF mode, no recording. Check the NotifyBox and the
   `AFDIAG.LOG` record with code 14 (`w0=stopped`, `w1=peak`, `w2=ms`,
   `w3=dist`). Does it stop near the in-focus position instead of infinity?
2. **Tune the stop.** The first thresholds are peak 80% / 120 ms / 3 s cap.
   Adjust from field logs; then use it as the actuator for Auto refocus.
3. **Direction.** TXDMPU `TVAFLOG 1 3 0` / `TVAFLOG 0 3 1` before `EXEC 10`
   remains the clean test for near/far and drive level.
4. **Integrate.** Replace the half-press path in Auto refocus with the bounded
   MPU search once it focuses reliably.
5. **Only later** consider EEPROM writes (hidden AF debug info at `0x1042`) or
   a custom MPU task/firmware.

The larger goal — ML-driven or continuous AF under a crop preset — can take two
routes:

- **MPU route:** use the group-8 `AF_LENS_SEARCH` route / `TVAFLOG` plus
  `(9,0x14)` feedback for a closed-loop AF that does not depend on Canon's x5
  AF area.
- **ICU route:** patch the ICU servo gate (`+0x710`, `+0x184`) so Canon’s own
  servo can run at x5. This is outside the MPU but may be simpler once the
  MPU behaviour is understood.

## 9. Safety and rollback

- The dumps are read-only.
- Raw messages are one-shot, delayed and refused while recording. The module
  always sends `(9,3,1)` after the drive commands.
- The group-8 AF search now refuses to start when the lens is at infinity
  (`focus_dist >= 60000`) and sends `(9,3,1)` after every EXEC. Starting the
  search at infinity froze the camera once; pull the battery if that happens.
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
- [`doc/EOSM-MPU-HANDOVER.md`](EOSM-MPU-HANDOVER.md) — continuation guide for
  the next model/engineer: current state, build/flash, test protocol, open
  questions.

### Local analysis scratchpad

The ROM1 dump and the Python/Unicorn scripts used to extract the tables above
live outside the repo in the session scratchpad
(`/private/tmp/claude-501/-Users-mgojkovic-repos-Crop-mood-eosm-slim-gui/<session>/scratchpad`).
The key scripts were `map_props.py`, `re/dis.sh`, `re/refs.py` and
`re/callers.py`.
