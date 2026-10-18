# EOS M MPU debug monitor over UART: connector, wiring, commands

Camera: Canon EOS M, firmware 2.0.2, EF-M 18-55 STM.
MPU: Renesas F74964A, SH2A, big-endian (same family as the 5D3 F74965A).
Companion doc: [`EOSM-MPU.md`](EOSM-MPU.md) (protocol, `mpu_lab`, ROM dump).

**Purpose.** This is a practical guide to the *serial debug port* of the EOS M
MPU — the port that exposes the `MON>>>` debug monitor and its command table
(`AF_LENS_SEARCH`, `TVAFLOG`, `VLENS_SET`, `VLENS_DRIVE`, `ABOUT_LENS`, `EEPR`,
`MDUMP`, ...). It is not a guide to the normal ICU↔MPU intercom; for that see
[`EOSM-MPU.md`](EOSM-MPU.md) §2–§5.

**Status legend** used below:

| Tag | Meaning |
|---|---|
| **[EXACT]** | Directly documented by a photo/log/ROM dump; safe to rely on |
| **[LIKELY]** | Same-family evidence (5D3/70D/EOS M2/M3), not yet confirmed on an EOS M |
| **[INFERRED]** | Derived from EOS M MPU ROM disassembly; argument layout known, semantics partly unclear |
| **[UNKNOWN]** | Not documented; do not guess on hardware you care about |

**Do this read-only first.** Connect only GND + TXDMPU, log the monitor, run the
read-only commands in §8. Do not run any lens/shutter/EEPROM-write command until
you have read §9–§11.

---

## 0. TL;DR

| Item | Result | Confidence |
|---|---|---|
| Service connector on EOS M | **JST XSR 6-pin** (0.6 mm pitch), not the 8-pin FZC of DIGIC 8+ | [EXACT] ML wiki |
| Number of connectors on the EOS M mainboard | **Two** 6-pin XSR-style connectors: one by the ICU pads, one by the MPU pad | [LIKELY] annotated mainboard photo (two 6-pin connectors visible; exact part series not proven) |
| MPU TX test pad | **TXDMPU**, bottom right of mainboard next to battery compartment | [EXACT] annotated photo |
| MPU UART levels | **3.3 V**, 115200 baud, 8N1, no flow control | [EXACT] annotated photo ("TXDMPU 115200 8N1 3.3V") |
| ICU UART levels | TXDICU/RXDICU **1.8 V**, 115200 8N1 | [EXACT] annotated photo + a1ex |
| Boot prompt | `\nMON>>>` then `\nE1OFF\n` | [EXACT] ROM strings 0x15470/0x15479 + photo |
| Exact EOS M XSR pin numbers | **Not published**; verify by probing | [UNKNOWN] |
| Likely XSR 6 pinout | 1 ICU TX, 2 ICU RX, 3 GND, 4 VCC, 5 MPU TX, 6 MPU RX (70D/M2 pattern) | [LIKELY] |
| How to send commands | Identify RXDMPU pad/pin and drive it with a 3.3 V TTL UART TX through ~1 kΩ | [LIKELY] procedure |
| Does EOS M have `/MPU_FORCED_WAKEUP`? | Not documented on Digic 5; camera must be powered normally | [LIKELY] |
| Monitor commands | 49 + 6 + 3 names in three ROM tables; **no `HELP`, no `MEMR`, no `MEMW`** | [EXACT] ROM |
| Numeric arguments | parsed as **hex** (`%lx`); `EEPR 0 1000` = 0x1000 | [EXACT] ROM |
| EEPROM command | `EEPR <hex start> [<hex end>]`; `EEPW` writes — never use it | [EXACT] ROM |
| AF search command | **`EXEC 10`** on the monitor (hex index 0x10 = 16 → handler 0x33794); typing `AF_LENS_SEARCH` is a handler-convention bug — do not use it | [EXACT] ROM + companion RE doc |
| TVAF log command | `TVAFLOG <0|1> <1|2|3> <0|1>` (hex args); **may start scan/log** | [EXACT] ROM |

---

## 1. What the port is

The EOS M MPU runs a Canon "Debug Monitor" (banner `Debug Monitor (K269)` at
ROM 0x14724; the visible text starts at 0x14725) with a line-oriented serial
console. The monitor is a normal MPU task: it prints a prompt, reads
CR/LF-terminated lines, uppercases tokens,
splits them on space/comma/tab, and dispatches to the command tables. The EOS M
ICU ROM also has its own separate UART/serial console, which is a different
port and a different prompt.

Observed monitor banner and prompt strings in the dumped EOS M MPU ROM:

```
0x14725  "             Debug Monitor (K269)              "
0x15470  "\nMON>>>"
0x15479  "\nE1OFF\n"
0x1546C  " ,\t"                 ; token delimiters
0x145A4  "%lx"                 ; numeric argument conversion
```

The monitor command tables were re-derived from the ROM for this doc; they
differ slightly from the first-pass layout in [`EOSM-MPU.md`](EOSM-MPU.md) §5.5
(see §12 and §13).

---

## 2. Physical access on the EOS M

The EOS M is a DIGIC 5 body, so the DIGIC 8+/DIGIC X 8-pin service connector
documented in the
[DIGIC 8+ MPU investigation thread](https://www.magiclantern.fm/forum/index.php?topic=25661.0)
does **not** apply. That 8-pin connector (VCC_ICU, RX_ICU, TX_ICU, GND,
`/MPU_FORCED_WAKEUP`, TX_MPU, RX_MPU, VCC_MPU) is for the later Toshiba/ARM MPU
generation and is useful here only as naming precedent.

The ML wiki's
[UART connectors table](https://wiki.magiclantern.fm/uart_connectors) lists
**EOS M = XSR 6** (JST XSR series, 0.6 mm pitch, 6 contacts), with the PCB
reference pointing at
[this forum post](https://www.magiclantern.fm/forum/index.php?topic=7531.msg211984).
EOS M2 (same connector family) is XSR 6 as well; the 5D3 is a 12-pin SUR and the
DIGIC 8+ bodies are 8-pin FZC — do not try to use those pinouts on the EOS M.

### 2.1 Access

There is **no externally accessible rubber flap** documented for the EOS M (unlike
the 5D3, whose SUR12 sits under the thumb rubber). The EOS M connectors and test
pads are on the mainboard and require rear-cover disassembly. Use a proper
guide, e.g. [iFixit: Canon EOS M Disassembly](https://www.ifixit.com/Guide/Canon+EOS+M+Disassembly/132419),
and follow the ESD/disassembly rules in §11.

### 2.2 Where the pads and connectors are

The best published evidence is Daniel Rozsnyo's annotated EOS M mainboard photo,
linked by a1ex in the
[Battery grip pins / UART thread](https://www.magiclantern.fm/forum/index.php?topic=7531.0):

- Photo (annotated): <https://a1ex.magiclantern.fm/bleeding-edge/EOS-M/canon-eos-m_serial-ports.jpeg>
- Archive copy: <https://web.archive.org/web/2024im_/https://a1ex.magiclantern.fm/bleeding-edge/EOS-M/canon-eos-m_serial-ports.jpeg>

On that photo (rear cover removed, mainboard facing you, battery compartment at
the bottom):

| Feature | Location | Signals | Confidence |
|---|---|---|---|
| ICU UART connector | right-hand side of the board, near the SD-card/strap-lug end; white 6-pin connector | TXDICU, RXDICU, GND, VCC (order TBD) | [LIKELY] connector is there; pin order [UNKNOWN] |
| TXDICU / RXDICU test pads | immediately below/left of that connector, a vertical pair of round pads; `TXDICU` is the upper one, `RXDICU` the lower | 1.8 V logic, 115200 8N1 | [EXACT] photo annotation |
| MPU UART connector | bottom centre-right, directly above the battery-compartment latch area; white 6-pin connector | TXDMPU, RXDMPU, GND, VCC (order TBD) | [LIKELY] connector is there; pin order [UNKNOWN] |
| TXDMPU test pad | the circled pad just below that connector, left pad of the pair | 3.3 V logic, 115200 8N1 | [EXACT] photo annotation |
| RXDMPU | not annotated in the photo; the other pad of the same pair or one of the XSR pins | 3.3 V logic | [UNKNOWN] — see §4.2 |
| GND | mainboard shield, tripod-mount plate, hot-shoe base, or any known GND pad | 0 V | [EXACT] common practice |
| VCC | the 6-pin connectors carry a supply/sense pin; **do not feed power into it** | 1.8 V / 3.3 V | [LIKELY] |

The photo's terminal insets also show the two consoles:

- **TXDICU output** (1.8 V): Canon ICU boot log, e.g. `K... READY`,
  `[STARTUP] ... ICU Firmware Version 2.0.2`, `InitializeIntercom End(0x0)`.
- **TXDMPU output** (3.3 V): `MON>>>` / `E1OFF` — the MPU debug monitor.

TXDICU and TXDMPU are therefore easy to tell apart: only the MPU port prints
`MON>>>` (and the K269 monitor banner), the ICU port prints `K... READY` and
DryOS startup messages.

### 2.3 Connector part numbers

The board connector series is JST XSR, 6 contacts. For the same XSR 6 connector
used on the 70D/M2, ML users reported:

- board side: **SM06B-XSRS-ETB** (JST XSR, right-angle, 0.6 mm pitch)
- cable assembly: **A06XSR06XSR36R51B** (or 06XSR-36S housing plus wires)

Sources: [JST XSR datasheet](https://www.jst-mfg.com/product/pdf/eng/eXSR.pdf),
[70D/M2/750D pinout discussion](https://www.magiclantern.fm/forum/index.php?topic=7531.0)
(pages 2–3 of the thread). Treat the exact cable code as [LIKELY] for the EOS M
until you compare the footprint; the connector family/size is [EXACT] from the
wiki.

If you only need the monitor **output**, the `TXDMPU` test pad is easier and less
risky than soldering a connector: one fine probe/needle (or a soldered wire) on
the pad, plus GND.

---

## 3. Voltage, levels, baud, framing

| Signal | Logic level | Baud/format | Source |
|---|---|---|---|
| TXDMPU (MPU console TX, MPU→PC) | **3.3 V** | **115200 8N1, no flow control** | [EXACT] annotated EOS M photo: "TXDMPU 115200 8N1 3.3V" |
| RXDMPU (MPU console RX, PC→MPU) | 3.3 V | 115200 8N1, no flow control | [LIKELY] same console, same UART; pad not annotated |
| TXDICU (ICU console TX) | **1.8 V** | 115200 8N1 | [EXACT] photo annotation + a1ex: 1.8 V TXDICU worked into a 3.3 V FTDI RX |
| RXDICU (ICU console RX) | 1.8 V | 115200 8N1 | [EXACT] a1ex: used 2×470 Ω divider from a 3.3 V adapter |

The 115200 8N1 result is independently consistent with every other Digic 5 MPU
console documented by ML: 5D3 `TXDMPU` at 3.3 V/115200
([a1ex, 2019](https://www.magiclantern.fm/forum/index.php?topic=7531.0)),
50D at 3.3 V/115200, and 70D/M2 XSR6 at 3.3 V/115200. The EOS M photo is the
direct confirmation for this body.

Important level notes:

- **Do not use a 5 V TTL adapter / RS-232 adapter.** Use a 3.3 V USB-UART
  (CP2102, FT232RL/FT232RQ, CH340 at 3.3 V, etc.).
- **3.3 V adapter → 1.8 V RXDICU is not safe**, even if it sometimes works.
  Use a proper level shifter or a resistor divider (2×470 Ω, as a1ex did).
- **1.8 V TXDICU → 3.3 V adapter RX** is fine: the 1.8 V high level is still a
  valid high for most 3.3 V CMOS inputs and was confirmed working.
- **3.3 V adapter → 3.3 V RXDMPU** is the normal case; put a ~1 kΩ series
  resistor in the adapter-TX line while probing an unidentified pad/pin. This
  limits damage if you hit a wrong signal.
- **Never connect the adapter's VCC/3V3 to the camera.** The camera is powered
  from its own battery. The service connector's VCC pins are sense/supply pins;
  feeding them can back-power the board.
- **No flow control** (no RTS/CTS), no parity, 8 data bits, 1 stop bit.

---

## 4. Adapter and wiring

### 4.1 MPU console (the recommended path)

Use a 3.3 V USB-UART adapter. Pin naming below is from the **adapter's** point of
view (adapter TX is an output, adapter RX an input).

```
PC / USB                         EOS M mainboard
---------                        ---------------
USB-UART 3.3V
  TX  o----[ 1k ]----------------o RXDMPU   (candidate: the second pad of the
  |                              |           TXDMPU pair, or an XSR6 pin)
  RX  o--------------------------o TXDMPU   (test pad, 3.3V; exact)
  GND o--------------------------o GND      (shield / tripod plate / known pad)
  VCC o---- not connected
```

- `TXDMPU` pad → adapter RX. This alone gives you the monitor and the whole
  read-only command set, because the monitor echoes what you type only if it
  receives it — but for logging it is enough.
- `RXDMPU` → adapter TX through 1 kΩ. You need this to type commands.
- GND: use the mainboard/shield; the UART ground is not optional.
- If the adapter has a 3V3 output pin, leave it open.

#### 4.1.1 Finding RXDMPU safely

`TXDMPU` is the only MPU pad explicitly annotated in the published EOS M photo.
The second pad in that pair is the natural `RXDMPU` candidate [LIKELY]. To verify
without guessing blindly:

1. Connect GND and adapter RX→TXDMPU only. Power the camera. At 115200 you
   should see the `MON>>>` / `E1OFF` banner. This proves ground, baud and the TX
   side.
2. With the adapter TX idle-high (3.3 V) and a 1 kΩ series resistor, touch the
   *other* pad in the TXDMPU pair. Send `ABOUT\r` (hex 41 42 4F 55 54 0D).
3. If the monitor executes `ABOUT` (prints the monitor version/checksums), that
   pad is `RXDMPU`. If nothing happens, release it and power-cycle before
   trying the next candidate (XSR6 pins or other test pads).
4. Do not hold a probe on an unknown pad while powering the camera; connect
   after the monitor banner is visible, and disconnect before power-off.

A wrong pad can reset or hang the MPU. The recovery is always: disconnect UART,
pull the battery, wait a few seconds, reinsert (§11.3).

### 4.2 ICU console (sanity check / alternative)

```
PC / USB                         EOS M mainboard
---------                        ---------------
USB-UART 3.3V
  TX  o----[ 470R ]---+----------o RXDICU
                      |           |
                     [470R]       |
                      |           |
  GND o---------------+-----------o GND
  RX  o---------------------------o TXDICU   (1.8V output; usually reads OK at 3.3V)
  VCC o---- not connected
```

The divider values are from a1ex's EOS M report (2×470 Ω); a 1.8 V-level
translator is cleaner. The ICU port is useful to confirm your adapter/settings
work, and to read Canon's ICU boot log, but it does **not** expose the MPU
monitor.

### 4.3 XSR 6 candidate pinout (verify before trusting)

No EOS M-specific XSR pin map has been published. The closest documented map is
the 70D/EOS M2-family XSR 6 (same connector series, same Digic 5 generation).
The ML wiki and the [Battery grip pins / UART thread](https://www.magiclantern.fm/forum/index.php?topic=7531.0)
give, for the 70D, screen side → SD-card side:

| Pin | 70D function (candidate for EOS M) | Level | Adapter connection |
|---|---|---|---|
| 1 | ICU TX | 1.8 V | adapter RX |
| 2 | ICU RX | 1.8 V | adapter TX via divider |
| 3 | GND | 0 V | adapter GND |
| 4 | VCC (3.1 V off / 3.3 V on) | — | **do not connect** |
| 5 | MPU TX | 3.3 V | adapter RX |
| 6 | MPU RX | 3.3 V | adapter TX via 1 kΩ |

Source for the 70D mapping: the 70D pinout post in
[Battery grip pins / UART](https://www.magiclantern.fm/forum/index.php?topic=7531.0)
(pages 3–4). **On the EOS M the same signals are split across two 6-pin
connectors** (one by the ICU pads, one by the MPU pad), so the numbering above is
a *candidate ordering*, not a confirmed EOS M map. Verify with a multimeter
(GND continuity, 3.3 V on the supply pin) and with the TX-line test in §5 before
connecting anything to a pin you have not identified.

A 5D3 12-pin SUR reference (for comparison only, **not applicable pin-for-pin**):
TXDICU and TXDMPU are separate lines, 3.3 V/115200; `TXDMPU` prints `MON>>>` and
`E1OFF` (a1ex, March 2019). The EOS M's two XSR6 connectors are the Digic 5
equivalent of that arrangement.

---

## 5. Opening the monitor

1. Disassemble to the mainboard (§2, §11). ESD-safe.
2. Connect GND and adapter RX to `TXDMPU` (3.3 V adapter, 115200 8N1).
3. Start the terminal program *before* powering the camera.
4. Insert a charged battery and power the camera on. If the camera's battery
   door is open, the body may refuse to boot — close it or defeat the switch
   only for bench work.
5. Press Enter (`\r`). Expected output:

```
MON>>>
E1OFF
```

The `TITLE` command prints the banner block (ROM handler 0x6F334, string at
0x14724/0x14725); on some boots/firmware revisions the same block may also be
printed before the prompt:

```
-----------------------------------------------
             Debug Monitor (K269)
...
  MPU Ver...0x....
  MPU check sum...0x........
  EEP Ver...0x..
```

(Format strings for these lines are at ROM 0x14725/0x146f0/0x14759; the exact
values depend on your MPU firmware/EEPROM.)

6. To type commands you need the RX line (§4.1.1). The monitor echoes characters
   as the line editor receives them and executes on CR (LF is also accepted;
   the line editor handles backspace/delete).
7. If you see garbage: check GND, adapter level (3.3 V), baud (115200), and that
   you are on `TXDMPU` not `TXDICU`. If you see nothing: check the pad contact
   and that the camera really booted (battery door, card, lens).

The monitor is a live MPU task. It is available while the camera is on; it is
not available with the camera off, and there is no documented
`/MPU_FORCED_WAKEUP` pin on the EOS M.

---

## 6. Command language

From the dumped ROM (parser at 0x70A60 line editor, 0x71CAC tokenizer,
0x71D2A dispatcher, 0x71D78 table scan):

- Prompt: `\nMON>>>`.
- Line buffer: 60 bytes in RAM (`0xFFFD8748`); token pointers: 60 entries at
  `0xFFFD8784`; maximum 60 tokens.
- Line terminator: CR and/or LF. Backspace/delete handled by the line editor.
- Token delimiters: space, comma, tab (`" ,\t"` at 0x1546C).
- Tokens are uppercased before dispatch, so command names are case-insensitive.
- Numeric arguments are parsed with `sscanf("%lx")` — **hexadecimal**. `10` means
  0x10, not ten. Use `0x` prefixes if in doubt, e.g. `EEPR 0x0 0x100`.
- There is **no `HELP`**, no `MEMR`, no `MEMW`, no `REGR`/`REGW`, no `UARTBRD`
  in the EOS M monitor. Those names come from the completely different DIGIC 8+
  Toshiba/ARM monitor in topic 25661. For memory dumps use `MDUMP`; for EEPROM
  reads use `EEPR`.

Command names are dispatched from three tables; the monitor has no other
built-ins:

**Table A** (`0x147B0`, 49 entries): 3 internal S-record keys, then
`UPBNY`, `EEPW`, `EEPR`, `EXEC`, `TEST`, `RELEASE`, `SW`,
`DISPALLONOLC`, `DISPALLOFF`, `DISPBLINKOLC`, `DISPALLONILC`,
`DISPBLINKILC`, `DISPALLONSI`, `CHECKBUZZER`, `CHECKSELFLED`, `SHRUN`,
`SHREADY`, `SHCHARGE`, `SH1ST`, `SH2ND`, `SHUTTEROPEN`, `MRUP`, `MRDOWN`,
`MRCHARGE`, `CHECKEFST2`, `CHECKCCC`, `CHECKEXTFLASH`, `AF_LENS_SEARCH`,
`TVAFLOG`, `TVAFPRTSAF`, `TEMPINFO`, `INDIINFO`, `MAINLOG`, `ABOUT_LENS`,
`ON`, `OFF`, `SETDCDC`, `GETDCDC`, `GETCDCRESULT`, `STATE`, `POWERLEDON`,
`POWERLEDOFF`, `VLENS_SET`, `VLENS_DRIVE`, `SETENABLEGPS`,
`SETDISABLEGPS`.

**Table B** (`0x14474`, 6 entries): `MDUMP`, `MDUMPB`, `MDUMPW`, `MDUMPL`,
`MMOT`, `RAMMOT`.

**Table C** (`0x14650`, 3 entries): `TITLE`, `ABOUT`, `VER`.

The first three Table A entries are S-record keys (`S00F...`, `S315`) used by the
firmware-update/EEPROM paths, not human commands. Never type them.

Useful read-only/status commands and their status:

| Command | Arguments | What it does | Read-only? |
|---|---|---|---|
| `TITLE` | none | prints the monitor banner | yes |
| `ABOUT`, `VER` | none | monitor version, MPU version/checksum, EEP version | yes |
| `MDUMP` | `<hex start> <hex end>` | hex dump of MPU memory; with no args dumps a 256-byte window from the current pointer (RAM `0xFFF85098`) | yes |
| `MDUMPB`, `MDUMPW`, `MDUMPL` | same | byte/word/long dump | yes |
| `EEPR` | `<hex start> [<hex end>]` | EEPROM hex dump | yes |
| `ABOUT_LENS` | none | lens information | yes |
| `TEMPINFO` | none | temperature sensors | yes |
| `INDIINFO` | none | gyro/indicator info (may repeat) | yes |
| `SW` | none | switch states | yes |
| `STATE` | none | state info | yes |
| `GETDCDC` | none | DCDC regulator status | yes |
| `GETCDCRESULT` | none | last DCDC result | yes |
| `CHECKEFST2`, `CHECKCCC`, `CHECKEXTFLASH` | none | factory checks | yes |
| `TVAFPRTSAF` | none | AF print/log helper | yes-ish |
| `MAINLOG` | none | main log | yes-ish |
| `MMOT`, `RAMMOT` | none | motor info | yes |
| `VLENS_SET` | see §9.3 | sets the lens command block | no (sets state) |
| `VLENS_DRIVE` | see §9.3 | fills the block and executes lens path | **no — moves lens** |
| `AF_LENS_SEARCH` | none | **broken from the text console** (handler expects a pointer, gets `argc`; writes to ROM 0x1..0x4) — use `EXEC 10` instead | **no — do not type** |
| `EXEC` | `<hex index>` | calls handler table `0x1417C[index](0xFFF80000)`; `EXEC 10` = AF search, `EXEC 11`..`EXEC 1F` = other internal handlers | **expert only** |
| `TVAFLOG` | `<0|1> <1|2|3> <0|1>` | sets scan/log parameters; may start scan logging | **no — may move lens** |
| `EEPW` | — | EEPROM write | **never** |
| `UPBNY`, S-record keys | — | firmware update | **never** |
| `TEST`, `RELEASE`, `SHRUN`, `SHREADY`, `SHCHARGE`, `SH1ST`, `SH2ND`, `SHUTTEROPEN`, `MRUP`, `MRDOWN`, `MRCHARGE` | — | shutter/mirror test | **do not use** |
| `SETDCDC`, `ON`, `OFF`, `POWERLEDON`, `POWERLEDOFF`, `DISP*` | — | power rails / LEDs / display | **do not use** |
| `SETENABLEGPS`, `SETDISABLEGPS` | — | GPS enable | do not use |

Command table addresses and handler addresses for reference:

```
Table A base 0x147B0, record 0x28 bytes: name[0x24] + handler[4] @ +0x24,
terminator at 0x14F58.
  EEPW           0x6EB7C     EEPR          0x6EAF4
  EXEC           0x6F526     UPBNY         0x713A6
  AF_LENS_SEARCH 0x33794     TVAFLOG       0x6F81A
  CHECKEXTFLASH  0x74D3E     TEST          0x6F8FC
  TVAFPRTSAF     0x6F8C6     ABOUT_LENS    0x206FE
  VLENS_SET      0x6FCD2     VLENS_DRIVE   0x6FDAC
Table B base 0x14474: MDUMP/B/W/L -> 0x6EE74, MMOT -> 0x6F220,
  RAMMOT -> 0x6F29A.
Table C base 0x14650: TITLE -> 0x6F334, ABOUT/VER -> 0x6F35E.
```

(These correct the first-pass table in [`EOSM-MPU.md`](EOSM-MPU.md) §5.5, where
the record layout was taken as handler-first and several handler addresses were
therefore shifted by one record.)

---

## 7. Terminal setup

Any of these work; the important part is 115200 8N1, no flow control, 3.3 V
levels.

```bash
# macOS/Linux, pick your device
picocom -b 115200 -d 8 -p n -y n /dev/tty.usbserial-XXXX
# or
screen /dev/tty.usbserial-XXXX 115200
# or
minicom -D /dev/tty.usbserial-XXXX -b 115200 -8
```

`screen` users: type commands followed by Enter; the monitor's echo may look
duplicated because both the line editor and your terminal echo the characters.
`picocom` is usually cleaner.

Python (pyserial) is the most reproducible:

```python
import serial, time

PORT = "/dev/tty.usbserial-XXXX"     # your adapter
ser = serial.Serial(PORT, 115200, bytesize=8, parity="N",
                    stopbits=1, timeout=0.25)
ser.reset_input_buffer()

def read_all(t=0.4):
    time.sleep(t)
    data = ser.read(16384)
    return data.decode("ascii", errors="replace")

def cmd(line, wait=0.5):
    ser.reset_input_buffer()
    ser.write((line + "\r").encode("ascii"))
    out = read_all(wait)
    print(f"MON>>> {line}\n{out}")
    return out

# wake the prompt / see the banner
ser.write(b"\r")
print(read_all(0.5))
```

Do **not** write a command and immediately close the port; the monitor needs the
line to be terminated with CR and a little time to print. 300–800 ms per command
is enough for short outputs; memory dumps take longer.

Example session (read-only):

```
MON>>>
E1OFF

MON>>> TITLE
-----------------------------------------------
             Debug Monitor (K269)
...
MON>>> ABOUT
...
  MPU Ver...0x....
  MPU check sum...0x........
  EEP Ver...0x..
MON>>> ABOUT_LENS
...
MON>>> MDUMP 0 80
00000000: ...
...
MON>>> EEPR 0 FF
00000000: ...
```

(Exact formatting/values depend on the MPU firmware and EEPROM contents; the
command names and the hex argument convention are what matter.)

---

## 8. Read-only scripts (start here)

### 8.1 Minimal read-only check

```python
import serial, time
ser = serial.Serial(PORT, 115200, bytesize=8, parity="N",
                    stopbits=1, timeout=0.25)

def run(line, wait=0.7):
    ser.reset_input_buffer()
    ser.write((line + "\r").encode())
    time.sleep(wait)
    print(ser.read(65536).decode("ascii", errors="replace"))

ser.write(b"\r"); time.sleep(0.5)
print(ser.read(4096).decode("ascii", errors="replace"))

for c in ["TITLE", "ABOUT", "VER", "ABOUT_LENS",
          "MDUMP 0 80", "EEPR 0 FF"]:
    run(c)
```

If `TITLE`/`ABOUT`/`MDUMP`/`EEPR` work, you have a working monitor and full
read-only access. `MDUMP` addresses are byte addresses in the MPU memory map
(firmware at 0x0, RAM at 0xFFF8xxxx; see [`EOSM-MPU.md`](EOSM-MPU.md) §5.5).
`EEPR` addresses are EEPROM byte addresses. Both end arguments are inclusive in
the handlers.

Take a full EEPROM backup before any write experiment:

```python
run("EEPR 0 7FF", wait=3.0)   # 2 KB range; adjust to the size you measure
```

Only run this if you are logging to a file and have verified the range; the
handler prints a hex dump. It is read-only.

### 8.2 Logging to a file

```python
import serial, time
ser = serial.Serial(PORT, 115200, timeout=0.2)
log = open("eosm-mpu-monitor.log", "wb")
ser.write(b"\r")
end = time.time() + 5
while time.time() < end:
    chunk = ser.read(4096)
    if chunk:
        log.write(chunk); log.flush()
log.close()
```

Do not run commands you have not read the syntax for; the monitor has no
confirmation prompts and no undo.

---

## 9. AF / lens commands

The commands below are exact at the parser/handler level (ROM); the *effect* on
the lens is what makes them dangerous. Use a lens attached in a controlled way,
support the lens, and be ready to pull the battery.

### 9.1 `TVAFLOG` — TV-AF log / scan configuration

Usage string in ROM (0x150E9):

```
Usage: TVAFLOG [0|1] [DriveLevel] [Direction]
  0|1 - AllScanMode OFF/ON (0:OFF, 1:ON)
  DriveLevel - 1:low, 2:middle, 3:high
  Direction - Start direction (0:near, 1:far)
```

Handler 0x6F81A requires exactly 3 arguments (argc == 4) and parses each with
`%lx`; it calls 0x84A18 with the three values. Exact commands:

```
TVAFLOG 0 2 0        # AllScan OFF, drive level middle (2), start near
TVAFLOG 1 3 1        # AllScan ON,  drive level high (3), start far
```

All three arguments are hex. Values outside the documented ranges are not
validated by the handler in an obvious way; stick to 0/1 and 1/2/3.

This sets the scan/log controls used by the AF search path. It can start or
change an AF scan (and therefore move the lens) on a live camera. Do it only
after the read-only checks, with a lens attached and no recording in progress.

### 9.2 AF search — use `EXEC 10`, not `AF_LENS_SEARCH`

This is the one place where the monitor is not intuitive.

`AF_LENS_SEARCH` exists in the name table and its handler is 0x33794, but the
text-monitor dispatcher calls handlers as `handler(argc, argv)` (r4 = argc,
r5 = argv), while 0x33794 expects `r4` to be a **pointer** to the status block at
`0xFFF80000` (the same convention as the indexed normal-message handlers). If
you type `AF_LENS_SEARCH`, the handler receives `r4 = 1` (the argc) and its final
`*(uint8_t *)r4 = 0x10` writes to ROM address 0x1 — it does **not** run the AF
search, and writing to low ROM/vector space is not something you want to do.
**Do not type `AF_LENS_SEARCH`.**

The correct serial invocation is the `EXEC` command with the **hex index** of the
handler in the `0x1417C` table. `AF_LENS_SEARCH` is index **0x10 (16)**:

```
EXEC 10
```

`EXEC` (handler 0x6F526) parses its argument with `%lx` and calls
`0x1417C[index](0xFFF80000)`, so `EXEC 10` runs 0x33794 with the correct
argument, exactly like the normal-message path. Handler 0x33794:

- takes no input arguments from the payload/argv;
- writes the AF command mailbox `0xFFF825DA = 17` (0x11);
- runs the flag/delay sequence and stores `0x10` at `0xFFF80000`;
- returns the status in the status block.

`EXEC 11` (index 0x11 = 0x337DE) and `EXEC 12` (0x33896) reach the neighbouring
group/op message handlers; indices 64..127 are the AF/lens low-level entries.
Inspect the companion `doc/EOSM-MPU-ROM.md` §4–§6 before using any index other
than 0x10.

Notes:

- The AF search core is now known to also be reachable **without** the UART, via
  a normal ICU message: `(group=8, op=2, len=32)` with payload bytes 4..7 =
  `00 00 00 10` (big-endian index 16), i.e. `(8, 2, 32)` + zero payload with the
  index at offset 4. That is the lowest-risk way to try AF search from ML; see
  `doc/EOSM-MPU-ROM.md` §4.3. The UART route is still needed for `TVAFLOG`,
  `VLENS_*`, `EEPR`, and the read-only monitor commands.
- Watch the lens and the camera behavior; if anything looks wrong, power-cycle
  (§11.3).

### 9.3 `VLENS_SET` and `VLENS_DRIVE` — the lens command block

Both commands use the MPU lens-command block at RAM `0xFFF8256C` (the same block
mentioned in [`EOSM-MPU.md`](EOSM-MPU.md) §5.5).

`VLENS_SET` (handler 0x6FCD2) — stage a lens command without triggering it:

```
VLENS_SET <A> <B> <C> [<d0> <d1> ...]
```

- requires `argc >= 4` (command + at least 3 arguments), otherwise prints
  `"size err"` (0x1523C);
- parses `argv[1]`, `argv[2]`, `argv[3]` with `%lx`;
- rejects `argv[2] > 4`;
- parses up to `argv[2]` additional byte arguments from `argv[4..]` (also hex);
- fills the block at `0xFFF8256C`: `+0` = gate (bit 7 set), `+1` = `argv[1]`
  (command/parameter id), `+2` = `argv[2]` (byte count 0..4), `+3` = `argv[3]`
  (destination offset/address byte), `+4..` = the data bytes;
- does **not** itself call the lens command path; a consumer around
  0x302E0/0x30300 acts on the gate later. So `VLENS_SET` is "stage", and
  `VLENS_DRIVE` is "stage + trigger".

Example (staging, no motor trigger yet):

```
VLENS_SET 2 1 0 0
VLENS_SET 2 4 0 0 0 0 0
```

`VLENS_DRIVE` (handler 0x6FDAC) — stage and trigger the lens command path:

```
VLENS_DRIVE <b1> <b2> <b3> <b4> <b5> <b6> <b7>
```

- requires `argc >= 7` (command + 6), and the handler reads `argv[1..7]`;
  supply **all seven** values (the argv[7] read is one past the terminator for a
  6-argument line, so seven is the safe form);
- clears the block and sets `+1 = 2`; parses `argv[1..4]` with `%lx` and packs
  them as a big-endian 32-bit value at `+8` (`arg1<<24 | arg2<<16 | arg3<<8 |
  arg4`) — the lens position/drive value;
- parses `argv[5]`, `argv[6]`, `argv[7]` into block bytes `+12`, `+13`, `+14`;
- sets gate bit 6 and the lens command task then executes the staged command
  (for `+1 == 2` it calls the lens driver around 0x30BC4/0x30B56/0x30B30 and
  copies `+12..+14` to `0xFFF8253D..0x3F`).

Example (all-zero, still a real lens command — do not run casually):

```
VLENS_DRIVE 0 0 0 0 0 0 0
```

The exact meaning of `+12..+14` (speed / direction / timeout) is still inferred
([INFERRED]). Do not run this until you have the lens-driver analysis and a
bench lens you can sacrifice. A wrong command byte can drive the lens into a
hard stop or leave it in an error state; the MPU may recover on the next AF
attempt, but there is no guarantee.

For completeness, the old normal-channel commands `(9,0x13,4)` and `(9,0x01,5)`
(see [`EOSM-MPU.md`](EOSM-MPU.md) §4) are a separate, safer first experiment:
they are ordinary intercom messages and the ICU already has a stop path
(`(9,3,1)`). The monitor `VLENS_DRIVE` bypasses that stop path, so it is the
higher-risk option.

### 9.4 `ABOUT_LENS` — lens information

Handler 0x206FE takes no arguments and prints lens information (the same data
source used by the normal lens property path). Read-only.

```
ABOUT_LENS
```

### 9.5 Suggested order

1. Read-only: `TITLE`, `ABOUT`, `ABOUT_LENS`, `MDUMP`, `EEPR` (§8).
2. `TVAFLOG 0 2 0` — configure, observe, no search expected with AllScan OFF.
3. `TVAFLOG 1 2 0` — AllScan ON; watch the lens and the camera behavior.
4. `EXEC 10` — one-shot AF search (correct serial form; never type
   `AF_LENS_SEARCH`); power-cycle if anything looks wrong. Alternatively use
   the no-solder ICU message `(8,2,32)` with index 0x10 in payload bytes 4..7.
5. Only after the ROM/lens-driver work is complete: `VLENS_SET` / `VLENS_DRIVE`.

---

## 10. EEPROM commands and the write warning

`EEPR <hex start> [<hex end>]` reads MPU EEPROM and prints a hex dump. Both
arguments are hex; the single-argument form reads a single address (the handler
sets end = start). Start small:

```
EEPR 0 0
EEPR 0 FF
EEPR 0 7FF
```

`EEPW` writes EEPROM. **Do not use it.** A wrong value in the MPU EEPROM can
prevent the MPU from booting, and the EOS M has no documented user-accessible
MPU recovery mode. The MPU debug monitor does have `UPBNY`/S-record update
commands, but using them to recover from a bad EEPROM write is a research
project, not a recovery plan. Take a full `EEPR` dump first, and only consider
writes after the MPU ROM work in the companion MPU-ROM doc (`doc/EOSM-MPU-ROM.md`,
being written by the ROM/RE workstream) documents
the exact EEPROM map.

---

## 11. Safety, failure modes, recovery

### 11.1 ESD and disassembly

- Work on a grounded ESD mat with a wrist strap; touch the bare metal chassis
  before handling the mainboard.
- Remove the battery before disassembly, and do not power the board on the bench
  with the rear cover off unless you are sure nothing can short on the metal
  chassis. Insulate the board from the chassis with card/tape if needed.
- Do not pull flex cables by the wires; release the connector latches.
- The EOS M has small 0.6 mm-pitch connectors; use magnification and fine probes.
  A slipped probe can bridge adjacent pins.
- If you solder: lead-free, low temperature, short dwell; do not lift pads.

### 11.2 Electrical

- 3.3 V TTL only; no 5 V, no RS-232 levels.
- Never feed VCC into the camera's service connector; power from the camera
  battery only.
- Series ~1 kΩ in the adapter TX line while probing unknown pads.
- Common ground between adapter and camera is required.
- Do not connect or disconnect the UART while the camera is writing to the card
  or during a firmware update.

### 11.3 If the camera hangs

The monitor commands are not transactional and there is no undo. If the camera
stops responding, the lens runs away, or the display goes black:

1. Disconnect the UART adapter (do not leave TX driving the camera).
2. Pull the battery, wait 5–10 s.
3. Reinsert the battery and power on normally, without the UART connected.
4. If it boots normally, the monitor state is gone (the MPU monitor has no
   persistent settings).
5. If it does not boot, remove the lens and card, retry; if it still does not
   boot, stop and treat it as a hardware/service issue. Do not try `UPBNY` or
   `EEPW` to "fix" it.

If you only ever used read-only commands (`TITLE`, `ABOUT`, `MDUMP`, `EEPR`,
`ABOUT_LENS`, `TEMPINFO`, `SW`), there is no persistent change to recover from.

### 11.4 Commands to treat as forbidden

`EEPW`, `UPBNY`, the S-record keys (`S00F...`, `S315`), `EXEC`, `TEST`,
`RELEASE`, `SHRUN`, `SHREADY`, `SHCHARGE`, `SH1ST`, `SH2ND`, `SHUTTEROPEN`,
`MRUP`, `MRDOWN`, `MRCHARGE`, `SETDCDC`, `ON`, `OFF`, `POWERLEDON`,
`POWERLEDOFF`, `DISP*`, `SETENABLEGPS`, `SETDISABLEGPS`, and (until the lens
driver is mapped) `VLENS_SET`/`VLENS_DRIVE`.

---

## 12. Fallbacks if the connector/pads are not usable

1. **ICU-side, no soldering.** The existing `mpu_dump` module /
   `MpuMonRead` path (group 8) reads MPU ROM/EEPROM/RAM through the normal
   intercom. This is already proven on the EOS M and is the safest way to get
   data. Group 8 op 2 can also call indexed handler 0x10 (`AF_LENS_SEARCH`)
   without the UART — see `doc/EOSM-MPU-ROM.md` §4.3 — but `TVAFLOG`,
   `VLENS_*`, `EEPR` output and the read-only monitor commands still need the
   serial console. See [`EOSM-MPU.md`](EOSM-MPU.md) §5.1 and
   `modules/mpu_dump`.

2. **ICU UART.** If resoldering the MPU connector is too risky, first practice
   on the ICU UART (TXDICU/RXDICU, 1.8 V, §4.2). If you can read the ICU console,
   your adapter, ground and settings are correct; the MPU pad is then just a
   matter of contact.

3. **Other test pads.** The EOS M mainboard photo shows the MPU test pad pair;
   look for unpopulated pads near the XSR connectors with a multimeter. A pad
   that sits at 3.3 V and toggles at boot is a TX candidate; a pad that sits at
   3.3 V and does not toggle is an RX candidate. Never probe with the camera on
   and a floating adapter TX.

4. **USB / remote port.** These are not the MPU debug UART. USB is PTP; the
   remote port is Canon's remote interface. Do not expect `MON>>>` there.

5. **Service flex / service manual.** No EOS M service flex is documented for
   this UART, and the EOS M service manual is not publicly available. Treat the
   test pads and XSR connectors as the only documented route.

6. **Passive only.** If you cannot identify RXDMPU safely, you can still log
   `TXDMPU` with adapter RX only (GND + RX). That gives you the monitor banner
   and any output the MPU prints on its own (some commands print without input),
   but you cannot type commands.

---

## 13. ROM/RE evidence and corrections

This doc was cross-checked against the canonical 768 KB EOS M MPU dump
(`card30/MPU-ROM.BIN`, see [`EOSM-MPU.md`](EOSM-MPU.md) §5.5). Relevant
addresses (all ROM, file offset = address because the dump is mapped at 0):

| Item | Address | Notes |
|---|---|---|
| Monitor banner | 0x14724 | `Debug Monitor (K269)` |
| Prompt | 0x15470 | `\nMON>>>` |
| Boot/status string | 0x15479 | `\nE1OFF\n` |
| Token delimiters | 0x1546C | `" ,\t"` |
| Numeric format | 0x145A4 | `%lx` (hex parse) |
| Command table A | 0x147B0 | 49 entries, record 0x28: name[0x24] + handler @ +0x24 |
| Command table B | 0x14474 | 6 entries, `MDUMP`/`MDUMPB`/`MDUMPW`/`MDUMPL`/`MMOT`/`RAMMOT` |
| Command table C | 0x14650 | 3 entries, `TITLE`/`ABOUT`/`VER` |
| Table pointer list | 0x1545C | `{0x147B0, 0x14650, 0x14474, 0}` |
| Line editor | 0x70A60 | line buffer RAM 0xFFFD8748 |
| Tokenizer | 0x71CAC | token pointers RAM 0xFFFD8784, max 60 tokens |
| Dispatcher | 0x71D2A | scans the table pointer list |
| Table scan/compare | 0x71D78 | case-insensitive after uppercase pass |
| Prompt print | 0x733E8 | calls putchar 0x70DC4 |
| Console putchar | 0x70DC4 | waits on RAM flags at 0xFFF8510A, calls UART channel write 0x3C21E |
| Console getchar | 0x70DF0 | reads UART channel 0xFFFE8000-based block |
| TVAFLOG handler | 0x6F81A | requires argc 4; calls 0x84A18 |
| TVAFLOG usage string | 0x150E9 | `Usage: TVAFLOG [0|1] [DriveLevel] [Direction]` |
| AF_LENS_SEARCH handler | 0x33794 | no input args; writes mailbox 0xFFF825DA=17; call via `EXEC 10` or group-8 index 16, **not** by name |
| CHECKEXTFLASH handler | 0x74D3E | 64-byte buffer, calls 0x1C1F8 (external-flash checks) |
| Indexed handler table | 0x1417C | index 0x10 = 0x33794 (AF search); `EXEC <hex index>` calls it |
| VLENS_SET handler | 0x6FCD2 | block 0xFFF8256C; stage only |
| VLENS_DRIVE handler | 0x6FDAC | 7 hex args, then lens path |
| EEPR handler | 0x6EAF4 | hex start/end, calls 0x6ED7C |
| EEPW handler | 0x6EB7C | write; never use |
| ABOUT_LENS handler | 0x206FE | no args |
| MDUMP handler | 0x6EE74 | hex start/end, current pointer 0xFFF85098 |

Corrections to [`EOSM-MPU.md`](EOSM-MPU.md) §5.5 (the first-pass table scan):

- The table layout is **name first** (36-byte NUL-padded name at +0, handler
  pointer at +0x24), not `[handler][name]` at 0x14824. The table starts at
  0x147B0; 0x14824 is the middle of the table, which is why the first-pass
  listing looked shifted.
- Correct handler pairs include `EEPR=0x6EAF4`, `EEPW=0x6EB7C`,
  `EXEC=0x6F526`, `TEST=0x6F8FC`, `TVAFLOG=0x6F81A`, `TVAFPRTSAF=0x6F8C6`,
  `ABOUT_LENS=0x206FE`, `VLENS_SET=0x6FCD2`, `VLENS_DRIVE=0x6FDAC`.
  `AF_LENS_SEARCH` is **0x33794**; 0x74D3E is `CHECKEXTFLASH`.
- Typing `AF_LENS_SEARCH` is a handler-convention bug: 0x33794 expects a
  pointer in r4 but the text dispatcher passes `argc`. Use `EXEC 10` instead
  (or the group-8 message in `doc/EOSM-MPU-ROM.md` §4.3).
- The EOS M monitor has no `HELP`/`MEMR`/`MEMW`/`REGR`/`REGW`/`UARTBRD`
  commands; those belong to the DIGIC 8+ (Toshiba/ARM) monitor in topic 25661.
  Use `MDUMP <hex start> <hex end>` and `EEPR <hex start> [<hex end>]`.

The UART hardware block and console registers seen in the ROM disassembly are
at `0xFFFE8000` (per-channel UART registers, channel stride 0x800; TX data +0x0C,
RX data +0x14, control +0x10, status +0x1C) and the console state flags are in
RAM at `0xFFF85108`/`0xFFF8510A`. This is useful if the baud ever needs to be
changed in a custom MPU build; it is **not needed** for the normal EOS M monitor
because the shipped firmware is already 115200 8N1.

---

## 14. Sources

### Magic Lantern forum / wiki

- [MPU communication (topic 17596)](https://www.magiclantern.fm/forum/index.php?topic=17596.0)
- [Battery grip pins / UART (topic 7531)](https://www.magiclantern.fm/forum/index.php?topic=7531.0) —
  EOS M/M2 XSR connectors, 70D pinout, 5D3/50D/70D UART levels and 115200
- [DIGIC 8+ MPU investigation (topic 25661)](https://www.magiclantern.fm/forum/index.php?topic=25661.0) —
  8-pin FZC service connector, `MON>>>` prompt and monitor command list (DIGIC 8+/ARM;
  not the EOS M monitor)
- [Magic Lantern wiki: UART connectors](https://wiki.magiclantern.fm/uart_connectors) —
  EOS M = XSR 6, cable/connector series links
- [Magic Lantern wiki: Tx19a](https://magiclantern.wikia.com/wiki/Tx19a)

### EOS M hardware

- [a1ex: EOS M UART pins, annotated mainboard photo](https://a1ex.magiclantern.fm/bleeding-edge/EOS-M/canon-eos-m_serial-ports.jpeg) —
  TXDICU/RXDICU 1.8 V, TXDMPU 3.3 V/115200 8N1, `MON>>>`/`E1OFF`
- [iFixit: Canon EOS M Disassembly](https://www.ifixit.com/Guide/Canon+EOS+M+Disassembly/132419)
- [JST XSR series datasheet](https://www.jst-mfg.com/product/pdf/eng/eXSR.pdf)
- [CHDK: EOS M3 UART pins](https://chdk.setepontos.com/index.php?topic=12542.msg129346) —
  same-generation XSR6 connector practice

### Repo

- [`doc/EOSM-MPU.md`](EOSM-MPU.md) — MPU protocol, ROM dump, `mpu_lab`
- `doc/EOSM-MPU-ROM.md` — MPU ROM monitor parser and command call graph
  (companion RE doc, being written in the same workstream)
- [`modules/mpu_dump`](../modules/mpu_dump) — ICU-side read-only MPU dump
- [`modules/mpu_lab`](../modules/mpu_lab) — ICU-side raw message probe

### ROM dump

The canonical EOS M MPU dump used for the address checks is the session
scratchpad copy `card30/MPU-ROM.BIN` (786,432 bytes), described in
[`EOSM-MPU.md`](EOSM-MPU.md) §5.5. It is not stored in the repo; re-dump it with
`mpu_dump`/`mpu_lab` if needed.
