# OpenPGP applet fuzzer (host, ASan/UBSan)

Standalone security fuzzer for the dongle OpenPGP applet's pure parsers
(`openpgp_card_apdu`, the 0xDB extended-header-list import, the DO store, the
ISO7816 APDU parser). Built from the pentest of Phase 1 (2026-06-10). **Not**
wired into the normal `test/` runner — it is a separate, on-demand tool.

Threat model fuzzed: a malicious USB host sending arbitrary / mutated APDU byte
sequences. Covers G4 (memory safety) and spot-checks G1 (key non-extraction),
G3 (PIN brute-force counter).

## Build & run

```sh
cd test/fuzz
gcc -std=c11 -g -O1 -fsanitize=address,undefined -DTEST_HOST \
    -I.. -I../../main/security \
    fuzz_openpgp.c \
    ../../main/security/openpgp_card.c \
    ../../main/security/openpgp_do.c \
    ../../main/security/apdu.c \
    ../../main/security/sec_confirm.c \
    -o fuzz_openpgp
./fuzz_openpgp
```

Expected: `Fuzz complete: ... 0 crashes.` and exit 0. Any ASan/UBSan abort prints
the triggering input. Re-run after touching any parser in `main/security/`.

Phases: random APDUs (unselected + selected), key-import mutations (length-field
corruption, B8 slot, extended-length, spurious template elements, truncation),
UIF-bypass attempts, G1 leak scan, 6E overflow guard, G3 retry-counter check.

# CCID transport fuzzer (host, ASan/UBSan)

`fuzz_ccid.c` fuzzes the layer *below* the applet: the USB CCID bulk framing in
`main/security/ccid.c` — the 10-byte header parse, the `dwLength` clamp, the
response builders and the ZLP (`% 64`) logic — i.e. the first bytes a malicious
host hits before any APDU reaches `openpgp_card_apdu()`. Built from the Phase-1/2
pentest (2026-06-11).

`ccid.c` is target-only (FreeRTOS + TinyUSB), so it is compiled against a host
stub soup: `ccid_host_stubs.h/.c` plus `ccid_stubs/` (fake `freertos/*`,
`device/usbd_pvt.h`, `esp_timer.h`, `esp_log.h`). `xSemaphoreGive()` and
`usbd_defer_func()` run synchronously, so a `PC_to_RDR_XfrBlock` drives the
extracted worker body (`ccid_process_xfrblock`) → `openpgp_card_apdu` →
response builder inline. The `usbd_edpt_xfer()` shim asserts every queued
transfer is ≤ 512 B (CCID_BUF_SZ), catching any response-length overflow.

`ccid.c` carries one behaviour-preserving change (extracting the worker loop
body into `ccid_process_xfrblock()`) plus a `#ifdef CCID_HOST_FUZZ` accessor
block (never built on target).

```sh
cd test/fuzz
gcc -std=c11 -g -O1 -fsanitize=address,undefined -DTEST_HOST -DCCID_HOST_FUZZ \
    -I. -Iccid_stubs -I../.. -I../../main/security \
    fuzz_ccid.c ccid_host_stubs.c \
    ../../main/security/ccid.c \
    ../../main/security/openpgp_card.c \
    ../../main/security/openpgp_do.c \
    ../../main/security/apdu.c \
    ../../main/security/sec_confirm.c \
    -o fuzz_ccid
./fuzz_ccid
```

Expected: `Fuzz complete: 200000 iterations, 0 crashes.` and exit 0.

Surface fuzzed: arbitrary `xferred` (0, runts 1..9, 10, 11..512, exact
64/128/256/512), arbitrary `msg_type` (PowerOn/Off/GetSlotStatus/XfrBlock +
garbage), arbitrary header `dwLength` (0, =avail, avail+1, 0xFFFFFFFF,
CCID_BUF_SZ, applet-bound), and the IN-completion / ZLP path. `xferred` is
capped at 512 because the controller cannot deliver more than the primed buffer
(see the faithfulness note in `fuzz_ccid.c`); the huge-length surface is fuzzed
via the `dwLength` header field instead, which is where the real clamp lives.
