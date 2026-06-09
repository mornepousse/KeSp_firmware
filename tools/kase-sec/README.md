# kase-sec

Rust CLI tool to provision secret slots on a KaSe dongle over USB CDC.

Talks to the dongle's existing `SEC_*` CDC commands (Plan 1 security foundation).
This is the host-side tool from Plan 2 — Task 7.

## What it does

The KaSe dongle stores up to 4 secret slots (HMAC-SHA1 keys for TOTP/HOTP).
`kase-sec` lets you provision, list, and clear those slots from a host machine
over the USB CDC serial connection using the binary KS/KR frame protocol.

Secrets are write-only: once provisioned, they cannot be read back out of the dongle.

## Build

```
cargo build --release
```

The binary ends up at `target/release/kase-sec`.

## Usage

### List provisioned slots

```
kase-sec --port /dev/ttyACM1 list
```

Output example:
```
IDX    TYPE         LABEL
--------------------------------------
0      hmac-sha1    github
2      hmac-sha1    work_vpn
```

### Provision a slot

Secret in hex (default):
```
kase-sec --port /dev/ttyACM1 provision --slot 0 --label github --secret 3132333435363738
```

Secret in base32 (RFC4648, standard for TOTP seeds — padding optional):
```
kase-sec --port /dev/ttyACM1 provision --slot 1 --label totp_work --secret GEZDGNBVGY3TQOJQ --format base32
```

Full options:
```
kase-sec --port /dev/ttyACM1 provision \
    --slot 0 \
    --label github \
    --secret <hex-or-base32-value> \
    --format hex|base32 \
    --type hmac-sha1
```

Constraints:
- `--slot`: 0 to 3
- `--label`: max 15 characters
- `--secret` (decoded): max 64 bytes
- `--type`: only `hmac-sha1` is supported for now

### Clear a slot

```
kase-sec --port /dev/ttyACM1 clear --slot 0
```

## Protocol notes

Frames use the KaSe binary CDC protocol:
- Request (KS):  `[0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]`
- Response (KR): `[0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]`

CRC-8 is computed over the payload bytes only (poly 0x31, init 0x00, MSB-first).

Command IDs: `SEC_SET_SLOT=0xC0`, `SEC_CLEAR_SLOT=0xC1`, `SEC_LIST=0xC2`.
