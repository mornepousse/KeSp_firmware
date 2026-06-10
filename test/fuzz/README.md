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
