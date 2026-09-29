# Chest link, S3 side — the left half as SPI master of Niphar_chest

Design of 2026-09-29. Counterpart of Niphar_chest's
`docs/superpowers/specs/2026-08-07-lien-s3-coffre-design.md` (the slave side,
implemented there in `main/link/link_spi.c` and `main/link/link_proto.{c,h}`,
flashed on the chest on 2026-09-05). That spec left "the S3 side in
`KeSp_firmware`" out of scope; this is it.

## 1. Goal

The chest (an ESP32-P4 inside the left half, powered by USB only) has no
source of physical presence: its OpenPGP / FIDO / OTP / OATH operations wait
for a confirmation that only a real key press on the keyboard may give. This
increment makes the left half:

1. **relay a real press** of `K_SEC_CONFIRM` to the chest while the chest has
   an operation pending;
2. **show the chest's state** on the left screen while it is present, and a
   **prompt** naming the pending operation.

Out of scope: driving the chest's options (USB mode, ISO image) — it needs the
data channel, specified but not implemented on the chest side; the right half
(no chest wiring); assigning `K_SEC_CONFIRM` in the default keymap (Mae places
it herself with KeSp_controller).

## 2. Pinout — verified against the netlist

Netlist exported with `kicad-cli sch export netlist` from
`Niphargus/hardware/pcb/niphar.kicad_sch` on 2026-09-29:

| net | S3 left (U6) | P4 (U16) | passive |
|---|---|---|---|
| `CS_P4` | pin 15, **GPIO3** | GPIO7 | **R48 10k to `/P4/P4_3V3`** (pull-up to the CHEST's rail) |
| `IRQ_P4` | pin 16, **GPIO46** | GPIO11 | R49 10k to GND (pull-down) |
| SCK / MISO / MOSI | 38 / 39 / 40 (shared with nRF24 + screen) | 9 / 10 / 8 | — |

Two documents were wrong and are corrected with this work:
`docs/NIPHARGUS_V2_HARDWARE.md` listed 3 and 46 as "forbidden — not wired",
and Niphar_chest's `docs/HARDWARE.md` (commit `1b0e481`) gave IO7/IO11 on the S3
side — those are the P4-side numbers. `test/test_niphar_left_pins.c` keeps 3
and 46 out of the matrix but stops calling them unwired.

### Electrical rules that follow

- **GPIO3 is driven only while the chest is powered.** R48 pulls CS up to the
  chest's rail, which is dead on battery: an S3 driving CS high (the idle,
  deselected level) into a dead rail pushes ~0.33 mA through R48 and back-powers
  the P4 through its protection diodes. Chest absent → GPIO3 is an input,
  no pull, and the SPI device is removed from the bus (its CS would otherwise
  idle high). R48 keeps CS deselected while the chest is powered and the S3
  is not driving (S3 boot, S3 reset).
- **GPIO46 is input-only** on the S3 and a strapping pin (ROM print control,
  "Ignored" with the default eFuse — ESP32-S3 TRM v1.8 table 8.3-1 p. 536, as
  the chest spec already established). R49 holds it low; the chest raises it
  only after the S3 has spoken once. Configured as input, no internal pull
  (R49 is the pull), rising-edge interrupt.
- **GPIO3** is the JTAG-source strap, ignored unless `EFUSE_STRAP_JTAG_SEL` is
  burned (it is not).

## 3. Presence

The chest is powered by the left half's USB (through its hub); on battery it
does not exist. **Presence = a USB host is present** (`usb_presence_cable()`,
the single USB presence rule). The sleep task already evaluates it every
second (`veille_task.c`); it hands the result to the link
(`chest_link_presence(bool)`), which wakes the link task on a change — **no new
poller on battery**.

Present → the task adds the SPI device and reads the registers until a valid
block appears (the P4 boots in ~1 s; a floating bus reads uniform 0x00/0xFF,
which is "absent", not an error — never logged). Absent → device removed,
GPIO3 released, state cleared, prompt gone.

A USB host present means the left half does not sleep (`VEILLE_VETO_USB`):
the link never has to survive a light sleep.

## 4. Transport

A third client of the shared SPI2 bus, every transaction under the radio
owner's lock (`rf_bus_lock`, like the screen). Mode 0 (the chest requires it:
SCK idle low). 1 MHz to start — the harness is short but three slaves share it;
raised only on a measurement.

The `spi_slave_hd` protocol (ESP-IDF `esp_spi_slave_protocol.rst`, "Supported
Commands"): 8-bit command, 8-bit address, 8 dummy cycles, then data —

| command | value | use |
|---|---|---|
| WRBUF | `0x01` | write the master's word (`0x10..0x13`) |
| RDBUF | `0x02` | read the chest's block (`0x00..0x0F`) |

The chest configures `command_bits = address_bits = dummy_bits = 8`; the master
sends the dummy phase on both commands, as Espressif's own master helper
(`essl_spi`) does.

**Reading**: RDBUF of the 20-byte block (or the 16 chest bytes), parsed by
`chest_proto_parse` — magic `NIPH`, version 1, CRC16 over `0x00..0x0B`
(`cr_crc16`, byte-identical in both repositories). Any failure → "no valid
state this round", nothing acted on.

**Confirming**: WRBUF of one byte `0x5A` at `0x10`. The chest clears it after
reading and increments `confirm_count`; the master considers a press
delivered when the count moves. If it has not moved after 200 ms (ten chest
ticks) and the same operation is still pending, the byte is written once
more; never a third time.

**Cadence** (all on USB power): a rising edge on GPIO46 wakes the task for an
immediate read; otherwise one read per 250 ms while present (the prompt must
appear within the chest's 15 s window, and the status line follows the chest).

## 5. The security invariant

**A confirmation is written only from a real key press.** The only caller of
`chest_gate_press()` (`main/comm/chest/chest_gate.c`) is the `K_SEC_CONFIRM` branch of
`key_processor.c`, gated by `is_new_press` (a held key confirms once). Never
from the CDC protocol, never from a timer, never from the CDC-reachable test
modes. Enforced by a `check.sh` rule: the symbol may appear only in
`main/input/key_processor.c`, `main/comm/chest/` and the tests.

`key_processor` does not write on the bus: it sets a request flag; the link
task performs the transaction. The request is dropped if no operation is
pending when the task runs.

**Routing of `K_SEC_CONFIRM`**: chest operation pending → the press goes to
the chest; otherwise it keeps its current meaning (`sec_confirm_authorize()`,
the dongle-side gate). One press, one destination.

## 6. Screen (left half)

Two additions to the memory-LCD model (`memlcd_model.h`, pure, tested):

- **Chest status**, in the top-left zone under the logo — the slot `zZ` uses
  only while asleep, and the left never sleeps while the chest exists. UNSCII 8,
  up to three short lines: `P4` (present; `P4..` while not READY), `SD` (card
  present), `USB` (a USB mode mounted on the host). Nothing when absent.
- **Prompt** while `pending_op != 0`: the bottom area (layer name + status
  lines) is replaced by the operation's label and `OK ?`, so the owner sees
  WHAT she authorizes before pressing. Labels, 6 characters max per line
  (`chest_op_label`, tested — the chest's own `sec_op_t` codes, pinned by
  value because the enum lives in the other repository):

| code | chest op | label |
|---|---|---|
| 1 | SIGN (PSO:CDS) | `SIGN` |
| 2 | DECRYPT | `DECRYP` |
| 3 | AUTH (INTERNAL AUTH) | `AUTH` |
| 4 | OTP (CR-HMAC) | `OTP` |
| 5 | FIDO_REGISTER | `FIDO +` |
| 6 | FIDO_AUTH | `FIDO` |
| 7 | OATH_CODE | `TOTP` |
| 8 | OATH_DELETE | `DELETE` |
| 9 | OATH_REPLACE | `REPLAC` |
| 10 | OATH_RESET | `RESET!` |
| other | unknown | `OP nn` |

A destructive operation is not rendered like a harmless one: the label is the
operation, not "confirm".

## 7. Contract with the chest

Niphar_chest published `docs/LINK_CONTRACT.md` (commit `8fd5d74`, 2026-09-29):
register map, parse order, confirmation handshake, version policy, and ten
vectors V1–V9 generated by the chest's own `link_proto.c` and pinned in its
`test/test_link_proto.c`. KeSp re-implements the parse (a few dozen lines) and
pins the same vectors verbatim in `test_chest_proto`, plus the CRC check value
(`0x6F91` over `"123456789"`: the function is CRC-16/MCRF4XX — the chest's
header calls it X-25, which would be `0x906E`; compare numbers, not names).
A change on one side that the other did not follow breaks a fast check.

The contract confirms this spec's choices: CS active LOW (the driver's
`spics_io_num` default — unlike the screen's active-high CS), mode 0, 8/8/8
command/address/dummy with the dummy phase on writes too, IRQ with a polling
floor, `0x5A`. It is wrong on one point: its §2 gives IO7/IO11 on the S3 side;
the netlist (§2 above) gives GPIO3/GPIO46 — IO7/IO11 are the P4-side numbers.
Reported to the chest session on 2026-09-29, which reproduced the netlist export and corrected its contract and HARDWARE.md the same day (Niphar_chest `4ce641a`).

## 8. Modules

```
main/comm/chest/
├── chest_proto.{c,h}   pure: parse, is_absent, op label, confirm retry rule
└── chest_link.{c,h}    transport + task: presence, SPI device, IRQ, reads,
                         confirm request; getters for the screen
```

Compiled on `niphar_left` only (`CONFIG_KASE_CHEST_LINK`, default y in its
`sdkconfig.defaults`, n elsewhere). `board.h` gains `BOARD_CHEST_CS 3`,
`BOARD_CHEST_IRQ 46`.

## 9. Error handling

- Chest absent / booting / hung: reads fail, status shows nothing (absent) or
  `P4..` (not READY); no log per failed read, one log per presence change.
- Bus busy (radio holds the lock): the read is skipped this round.
- Version ≠ 1: treated as "not a valid chest" (no prompt, `P4?` shown), logged
  once — a chest flashed ahead of the keyboard must not be half-understood.
- Confirm with nothing pending: dropped, as the chest would do.

## 10. Verification

Host (TDD, tests first): parse against the chest's vectors; absent on uniform
0x00/0xFF only; op labels; the confirm retry rule (write, count moved →
done; not moved after 200 ms and same op → one retry; never a third);
`K_SEC_CONFIRM` routing (pending → chest request, not pending → local gate);
the memlcd model diff on the new fields; the pin test (CS 3, IRQ 46, neither
in the matrix).

Bench (`[smoke:Chest link]`, new item): left on USB with the chest flashed —
status shows `P4 SD USB`; `gpg --card-status` then a signature: the prompt
shows `SIGN / OK ?`, a press on `K_SEC_CONFIRM` signs, no press → `6985` after
15 s; unplug USB → status gone, and on battery the sleep current is unchanged
(GPIO3 released — the R48 rule). Radio and screen keep working while the chest
is present (three slaves on the bus).
