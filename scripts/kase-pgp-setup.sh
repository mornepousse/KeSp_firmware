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

command -v gpg >/dev/null 2>&1 || die "gpg introuvable. Installe-le en natif (NixOS: programs.gnupg.agent + pkgs.gnupg). Voir docs/OPENPGP_CARD.md §0."

# Run an expect script file, transparently via nix-shell if expect is absent.
run_expect(){
  if have expect; then expect "$1"
  else warn "expect absent → via nix-shell"; nix-shell -p expect --run "expect '$1'"
  fi
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
  ensure_card || die "carte injoignable. Dongle branché ? Règle udev 303a ? (docs/OPENPGP_CARD.md §0). CCID parfois figé → réessaie."
}

card_has_sig_key(){ gpg --card-status 2>/dev/null | grep -qiE '^Signature key \.+: [0-9A-F]'; }

# ---------------------------------------------------------------- status
cmd_status(){
  require_card
  ok "carte détectée"
  gpg --card-status
}

# ---------------------------------------------------------------- reset
# Deterministic factory reset, independent of the current PINs: block both PINs
# (the OpenPGP §7.2.16 escape hatch) then TERMINATE + ACTIVATE → defaults.
cmd_reset(){
  require_card
  warn "FACTORY RESET — efface TOUTES les clés de la carte et remet les PINs par défaut."
  read -rp "Taper 'oui' pour confirmer : " a; [ "$a" = "oui" ] || die "annulé."
  info "Blocage des PINs puis terminate/activate…"
  # block PW1 (3 wrong VERIFY 0x81) + PW3 (3 wrong VERIFY 0x83), then E6 + 44.
  scd_apdu \
    "00A4040006 $AID_HEX" \
    "0020008106 303030303030" "0020008106 303030303030" "0020008106 303030303030" \
    "0020008308 3030303030303030" "0020008308 3030303030303030" "0020008308 3030303030303030" \
    "00E60000" "00440000" >/dev/null
  gpgconf --kill all >/dev/null 2>&1 || true; sleep 1
  require_card
  if gpg --card-status 2>/dev/null | grep -q 'PIN retry counter : 3 0 3' && ! card_has_sig_key; then
    ok "carte remise à zéro (PINs défaut: PW1=$PW1_DEFAULT PW3=$PW3_DEFAULT)."
  else
    die "reset incomplet — relance, ou fais 'gpg --card-edit → admin → factory-reset' à la main."
  fi
}

# ---------------------------------------------------------------- pins
cmd_pins(){
  require_card
  info "${c_bold}Changement des PINs${c_0} (défauts publics PW1=$PW1_DEFAULT / PW3=$PW3_DEFAULT)."
  info "Dans le menu : ${c_bold}admin${c_0} → ${c_bold}passwd${c_0} → 1 (PW1 user) → 3 (PW3 admin) → q → quit."
  warn "3 essais faux sur les DEUX PINs = carte bloquée (récup: $0 reset, qui efface tout)."
  read -rp "Entrée pour ouvrir gpg --card-edit… " _
  gpg --card-edit
}

# ---------------------------------------------------------------- generate
cmd_generate(){
  require_card
  if card_has_sig_key; then
    warn "Une clé de signature existe déjà sur la carte."
    read -rp "La remplacer ? Il faudra '$0 reset' d'abord. Continuer le reset ? (oui/non) " a
    [ "$a" = "oui" ] && cmd_reset || die "annulé."
  fi
  local name email
  read -rp "Nom complet (Real name) : " name
  read -rp "Email : " email
  [ -n "$name" ] && [ -n "$email" ] || die "nom et email requis."

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
  warn "${c_bold}IMPORTANT — la TOUCHE physique :${c_0}"
  warn "gpg va (1) te demander le PIN Admin puis User (fenêtre pinentry),"
  warn "puis (2) auto-signer le certificat avec la clé de signature → ${c_bold}presse K_SEC_CONFIRM${c_0}"
  warn "sur ta moitié dans les 15 s quand ça bloque. Sans la touche → échec (6985)."
  read -rp "Prête ? Entrée pour lancer la génération… " _

  if run_expect /tmp/.kase_gen.exp | tee /tmp/.kase_gen.out | grep -q '__TIMEOUT__'; then
    rm -f /tmp/.kase_gen.exp
    die "génération bloquée (timeout). PIN non saisi, ou touche non pressée à temps. Relance."
  fi
  rm -f /tmp/.kase_gen.exp
  if card_has_sig_key; then
    ok "identité générée sur la carte (signature + chiffrement + authentification)."
    cmd_git
    echo; cmd_ssh
  else
    die "génération non confirmée — vérifie la sortie ci-dessus (touche pressée à temps ?)."
  fi
}

# ---------------------------------------------------------------- git
cmd_git(){
  require_card
  local fpr
  fpr=$(gpg --list-keys --with-colons 2>/dev/null | awk -F: '/^fpr/{print $10; exit}')
  [ -n "$fpr" ] || die "aucune clé dans le trousseau — génère d'abord ($0 generate)."
  git config --global user.signingkey "$fpr"
  git config --global commit.gpgsign true
  git config --global gpg.program gpg
  ok "git signing configuré (clé $fpr). Chaque commit → PIN + touche. Vérif: git log --show-signature"
}

# ---------------------------------------------------------------- ssh
cmd_ssh(){
  require_card
  if [ -z "${SSH_AUTH_SOCK:-}" ]; then
    export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket 2>/dev/null)"
    gpgconf --launch gpg-agent >/dev/null 2>&1 || true
    warn "SSH_AUTH_SOCK n'était pas posé — fixé pour ce shell. Persistance: programs.gnupg.agent.enableSSHSupport + shell de login frais."
  fi
  gpg --card-status >/dev/null 2>&1
  local key
  key=$(ssh-add -L 2>/dev/null | grep -i 'cardno' | head -1)
  if [ -n "$key" ]; then
    ok "clé SSH de la carte :"
    echo "  $key"
    info "→ colle cette ligne dans GitLab → Preferences → SSH Keys (ou GitHub → Settings → SSH keys)."
    info "  Test ensuite : ssh -T git@gitlab.com"
  else
    warn "ssh-add ne montre pas de clé carte."
    info "  - carte sans clé AUTH ? → $0 generate"
    info "  - 'has no identities' = carte vierge ; 'no connection' = SSH_AUTH_SOCK/agent (voir §0, conflit gnome-keyring)."
  fi
}

# ---------------------------------------------------------------- setup
cmd_setup(){
  info "${c_bold}=== Assistant configuration carte OpenPGP KaSe ===${c_0}"
  cmd_status; echo
  read -rp "1/3 Changer les PINs maintenant ? (oui/non) " a
  [ "$a" = "oui" ] && { cmd_pins; echo; }
  read -rp "2/3 Générer ton identité SUR la carte ? (oui/non) " a
  [ "$a" = "oui" ] && { cmd_generate; echo; } || { warn "generate sauté — git/ssh nécessitent une clé."; return; }
  ok "3/3 Terminé. Identité dev complète sur le dongle."
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
kase-pgp-setup.sh — assistant carte OpenPGP du dongle KaSe
Usage: $0 <commande>
  status     détecte la carte (+ recovery CCID) et affiche son état
  setup      assistant complet : PINs → generate → git → SSH/GitLab
  generate   génère l'identité sur la carte (+ git + SSH)
  reset      remet la carte à zéro (PINs par défaut, efface les clés)
  pins       change les PINs (gpg --card-edit guidé)
  git        configure la signature git avec la clé de la carte
  ssh        affiche la clé SSH de la carte pour GitLab/GitHub
Doc: docs/OPENPGP_CARD.md §0
USAGE
     [ -z "${1:-}" ] && exit 0 || exit 1 ;;
esac
