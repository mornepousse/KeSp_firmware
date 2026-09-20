#!/usr/bin/env bash
# kase-pgp-setup.sh — guided OpenPGP setup wizard for the KaSe dongle.
#
# The dongle is a GnuPG OpenPGP smartcard (USB CCID, 303a:4001). This wizard
# orchestrates the fiddly parts of provisioning it and recovers the known
# gotchas (CCID wedge, blocked PINs). It deliberately does NOT touch two things,
# by design of the security model:
#   - your PINs        → entered through gpg's pinentry; never seen/stored here.
#   - the physical touch → during on-card key generation gpg self-signs the user
#                          ID with the SIGNATURE key (UIF Sign=on); you must press
#                          K_SEC_CONFIRM on a paired half when prompted.
#
# Commands:
#   status     detect the card, recover a wedged CCID, print its state
#   setup      full wizard: (PINs) → generate on-card → git signing → SSH/GitLab
#   generate   on-card key generation only (sign+decrypt+auth) + git + SSH
#   reset      factory-reset to a blank card with default PINs (handles blocked PINs)
#   ssh        re-print the card's SSH public key for GitLab/GitHub
#   git        configure git commit signing with the card's signature key
#
# Requires: gpg (native — see docs/OPENPGP_CARD.md §0), expect (auto-wrapped via
# nix-shell if absent). Prereq: programs.gnupg.agent with enableSSHSupport.
set -uo pipefail

PW1_DEFAULT="123456"
PW3_DEFAULT="12345678"
AID_HEX="D27600012401"

c_grn=$'\e[32m'; c_red=$'\e[31m'; c_yel=$'\e[33m'; c_bold=$'\e[1m'; c_0=$'\e[0m'
info(){ printf '%s\n' "$*"; }
ok(){   printf '%s✓%s %s\n' "$c_grn" "$c_0" "$*"; }
warn(){ printf '%s!%s %s\n' "$c_yel" "$c_0" "$*"; }
die(){  printf '%s✗ %s%s\n' "$c_red" "$*" "$c_0" >&2; exit 1; }
have(){ command -v "$1" >/dev/null 2>&1; }

command -v gpg >/dev/null 2>&1 || die "gpg not found. Install it natively (NixOS: programs.gnupg.agent + pkgs.gnupg). See docs/OPENPGP_CARD.md §0."

# Interactive read that first restores the terminal to a sane (cooked) mode, so
# a raw tty leaked by a prior expect/gpg session doesn't turn Enter into "^M".
# Honors KASE_YES=1 to auto-answer "oui" (non-interactive / CI).
ask(){  # ask "<prompt>" -> prints the reply on stdout
  if [ "${KASE_YES:-}" = "1" ]; then printf 'oui'; return; fi
  stty sane 2>/dev/null || true
  local r; read -rp "$1 " r </dev/tty; printf '%s' "$r"
}

# Run an expect script file, transparently via nix-shell if expect is absent.
# Always restore the tty afterwards (expect leaves it raw if interrupted).
run_expect(){
  if have expect; then expect "$1"
  else warn "expect missing -> via nix-shell"; nix-shell -p expect --run "expect '$1'"
  fi
  local rc=$?; stty sane 2>/dev/null || true; return $rc
}

# Send raw APDUs to the card in ONE gpg-connect-agent session (PIN-state holds).
# Args: each a hex APDU string. Echoes the D-lines.
scd_apdu(){
  local args=(/hex "scd serialno")
  local a; for a in "$@"; do args+=("scd apdu $a"); done
  args+=(/bye)
  gpg-connect-agent "${args[@]}" 2>/dev/null | grep '^D\['
}

# Recover a wedged CCID ("No such device" after heavy use) and confirm the card
# answers. Returns 0 if the card is reachable.
ensure_card(){
  local i
  for i in 1 2 3 4; do
    if gpg --card-status >/dev/null 2>&1; then return 0; fi
    gpgconf --kill all >/dev/null 2>&1 || true
    sleep 1
  done
  return 1
}

require_card(){
  ensure_card || die "card unreachable. Dongle plugged in? udev rule 303a set up? (docs/OPENPGP_CARD.md §0). CCID sometimes wedges -> retry."
}

card_has_sig_key(){ gpg --card-status 2>/dev/null | grep -qiE '^Signature key \.+: [0-9A-F]'; }

# ---------------------------------------------------------------- status
cmd_status(){
  require_card
  ok "card detected"
  gpg --card-status
}

# ---------------------------------------------------------------- reset
# Deterministic factory reset, independent of the current PINs: block both PINs
# (the OpenPGP §7.2.16 escape hatch) then TERMINATE + ACTIVATE → defaults.
cmd_reset(){
  require_card
  warn "FACTORY RESET — erases ALL keys on the card and restores the default PINs."
  # Safety interlock: if the card holds a real identity, refuse the easy/
  # automated path. KASE_YES / piped 'oui' must NEVER wipe a live identity —
  # that is exactly how the 2026-06-25 on-card identity (no backup) was lost
  # (`KASE_YES=1 reset` / `echo oui | reset` while testing the wizard).
  if card_has_sig_key; then
    local serial; serial="$(gpg --card-status 2>/dev/null \
                            | sed -n 's/^Serial number *[. ]*: *//p' | head -1)"
    warn "⚠ This card HOLDS an identity (a signature key is present)."
    warn "  The wipe is IRREVERSIBLE — make sure you have a backup first."
    if [ "${KASE_YES:-}" = "1" ]; then
      die "refused: KASE_YES will not wipe a card that holds keys. An interactive reset is required."
    fi
    stty sane 2>/dev/null || true
    local r; read -rp "To confirm, type the card's serial number (${serial:-?}): " r </dev/tty
    [ -n "$serial" ] && [ "$r" = "$serial" ] || die "wrong serial number — reset cancelled (card untouched)."
  else
    [ "$(ask "Blank card. Type 'oui' to confirm the reset:")" = "oui" ] || die "cancelled."
  fi
  info "Blocking the PINs then terminate/activate..."
  # block PW1 (3 wrong VERIFY 0x81) + PW3 (3 wrong VERIFY 0x83), then E6 + 44.
  scd_apdu \
    "00A4040006 $AID_HEX" \
    "0020008106 303030303030" "0020008106 303030303030" "0020008106 303030303030" \
    "0020008308 3030303030303030" "0020008308 3030303030303030" "0020008308 3030303030303030" \
    "00E60000" "00440000" >/dev/null
  gpgconf --kill all >/dev/null 2>&1 || true; sleep 1
  require_card
  if gpg --card-status 2>/dev/null | grep -q 'PIN retry counter : 3 0 3' && ! card_has_sig_key; then
    ok "card reset to zero (default PINs: PW1=$PW1_DEFAULT PW3=$PW3_DEFAULT)."
  else
    die "incomplete reset — rerun, or do 'gpg --card-edit -> admin -> factory-reset' by hand."
  fi
}

# ---------------------------------------------------------------- pins
cmd_pins(){
  require_card
  info "${c_bold}Changing the PINs${c_0} (public defaults PW1=$PW1_DEFAULT / PW3=$PW3_DEFAULT)."
  info "In the menu: ${c_bold}admin${c_0} -> ${c_bold}passwd${c_0} -> 1 (PW1 user) -> 3 (PW3 admin) -> q -> quit."
  warn "3 wrong attempts on BOTH PINs = blocked card (recovery: $0 reset, which erases everything)."
  ask "Press Enter to open gpg --card-edit..." >/dev/null
  gpg --card-edit
}

# ---------------------------------------------------------------- generate
cmd_generate(){
  require_card
  if card_has_sig_key; then
    warn "A signature key already exists on the card."
    read -rp "Replace it? '$0 reset' will be needed first. Proceed with the reset? (oui/non) " a
    [ "$a" = "oui" ] && cmd_reset || die "cancelled."
  fi
  local name email
  name=$(ask "Full name (Real name):")
  email=$(ask "Email:")
  [ -n "$name" ] && [ -n "$email" ] || die "name and email required."

  cat > /tmp/.kase_gen.exp <<EXP
set timeout 180
spawn gpg --card-edit
expect "gpg/card>"
send "admin\r"
expect "gpg/card>"
send "generate\r"
expect {
  -re {backup of encryption key.*\?} { send "n\r"; exp_continue }
  -re {Key is valid for\?}           { send "0\r"; exp_continue }
  -re {Is this correct\?}            { send "y\r"; exp_continue }
  -re {Real name:}                   { send "$name\r"; exp_continue }
  -re {Email address:}               { send "$email\r"; exp_continue }
  -re {Comment:}                     { send "\r"; exp_continue }
  -re {\(O\)kay/\(Q\)uit\?}          { send "O\r"; exp_continue }
  -re {created and signed}           { }
  -re {gpg/card>}                    { send "quit\r" }
  timeout                            { puts "\n__TIMEOUT__"; exit 2 }
}
expect eof
EXP
  echo
  warn "${c_bold}IMPORTANT — the physical TOUCH:${c_0}"
  warn "gpg will (1) ask you for the Admin PIN then the User PIN (pinentry window),"
  warn "then (2) auto-sign the certificate with the signature key -> ${c_bold}press K_SEC_CONFIRM${c_0}"
  warn "on your half within 15 s when it blocks. Without the touch -> failure (6985)."
  ask "Ready? Press Enter to start the generation..." >/dev/null

  if run_expect /tmp/.kase_gen.exp | tee /tmp/.kase_gen.out | grep -q '__TIMEOUT__'; then
    rm -f /tmp/.kase_gen.exp
    die "generation stuck (timeout). PIN not entered, or touch not pressed in time. Rerun."
  fi
  rm -f /tmp/.kase_gen.exp
  if card_has_sig_key; then
    ok "identity generated on the card (signature + encryption + authentication)."
    cmd_git
    echo; cmd_ssh
  else
    die "generation not confirmed — check the output above (touch pressed in time?)."
  fi
}

# ---------------------------------------------------------------- git
cmd_git(){
  require_card
  local fpr
  fpr=$(gpg --list-keys --with-colons 2>/dev/null | awk -F: '/^fpr/{print $10; exit}')
  [ -n "$fpr" ] || die "no key in the keyring — generate one first ($0 generate)."
  git config --global user.signingkey "$fpr"
  git config --global commit.gpgsign true
  git config --global gpg.program gpg
  ok "git signing configured (key $fpr). Every commit -> PIN + touch. Check: git log --show-signature"
}

# ---------------------------------------------------------------- ssh
cmd_ssh(){
  require_card
  if [ -z "${SSH_AUTH_SOCK:-}" ]; then
    export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket 2>/dev/null)"
    gpgconf --launch gpg-agent >/dev/null 2>&1 || true
    warn "SSH_AUTH_SOCK was not set — set for this shell. Persistence: programs.gnupg.agent.enableSSHSupport + a fresh login shell."
  fi
  gpg --card-status >/dev/null 2>&1
  local key
  key=$(ssh-add -L 2>/dev/null | grep -i 'cardno' | head -1)
  if [ -n "$key" ]; then
    ok "the card's SSH key:"
    echo "  $key"
    info "-> paste this line into GitLab -> Preferences -> SSH Keys (or GitHub -> Settings -> SSH keys)."
    info "  Then test: ssh -T git@gitlab.com"
  else
    warn "ssh-add shows no card key."
    info "  - card without an AUTH key? -> $0 generate"
    info "  - 'has no identities' = blank card; 'no connection' = SSH_AUTH_SOCK/agent (see §0, gnome-keyring conflict)."
  fi
}

# ---------------------------------------------------------------- setup
cmd_setup(){
  info "${c_bold}=== KaSe OpenPGP card setup wizard ===${c_0}"
  cmd_status; echo
  [ "$(ask "1/3 Change the PINs now? (oui/non)")" = "oui" ] && { cmd_pins; echo; }
  [ "$(ask "2/3 Generate your identity ON the card? (oui/non)")" = "oui" ] && { cmd_generate; echo; } \
    || { warn "generate skipped — git/ssh require a key."; return; }
  ok "3/3 Done. Full dev identity on the dongle."
}

case "${1:-}" in
  status)   cmd_status ;;
  setup)    cmd_setup ;;
  generate) cmd_generate ;;
  reset)    cmd_reset ;;
  pins)     cmd_pins ;;
  git)      cmd_git ;;
  ssh)      cmd_ssh ;;
  *) cat <<USAGE
kase-pgp-setup.sh — KaSe dongle OpenPGP card wizard
Usage: $0 <command>
  status     detects the card (+ CCID recovery) and prints its state
  setup      full wizard: PINs -> generate -> git -> SSH/GitLab
  generate   generates the identity on the card (+ git + SSH)
  reset      resets the card to zero (default PINs, erases the keys)
  pins       changes the PINs (guided gpg --card-edit)
  git        configures git signing with the card's key
  ssh        prints the card's SSH key for GitLab/GitHub
Doc: docs/OPENPGP_CARD.md §0
USAGE
     [ -z "${1:-}" ] && exit 0 || exit 1 ;;
esac
