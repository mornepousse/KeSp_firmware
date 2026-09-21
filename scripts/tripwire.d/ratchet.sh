#!/usr/bin/env bash
# Brique tripwire : ratchet de tests (.tripwire-testcount)
# Sourcee par scripts/check.sh si presente. La retirer = supprimer ce fichier.
# Ne rien appeler ici : la brique s'enregistre, le noyau appelle.

# Ratchet de tests (optionnel) : commande une-ligne qui imprime le nombre de
# tests. Vide -> ratchet inerte. Référence committée : .tripwire-testcount
# (la baisser = diff visible en review). Rouge au pre-push si le compte chute
# (TRIPWIRE_RATCHET_STRICT=1, posé par le hook pre-push).
TEST_COUNT_CMD="grep -rho 'TEST_ASSERT' test/ | wc -l"

tw_ratchet() {
  local r=0
  # ---- Ratchet de tests : le nombre de tests ne baisse jamais en silence ----
  if [ -n "$TEST_COUNT_CMD" ]; then
    TC="$( (eval "$TEST_COUNT_CMD") 2>/dev/null | tr -d '[:space:]' )"
    case "$TC" in ''|*[!0-9]*) TC="" ;; esac
    REF="$(cat .tripwire-testcount 2>/dev/null | tr -d '[:space:]')"
    case "$REF" in ''|*[!0-9]*) REF="" ;; esac
    if [ -n "$TC" ]; then
      if [ -z "$REF" ]; then
        printf '%s\n' "$TC" > .tripwire-testcount 2>/dev/null \
          && info "ratchet: référence initialisée à $TC tests (.tripwire-testcount — à committer)"
      elif [ "$TC" -gt "$REF" ]; then
        printf '%s\n' "$TC" > .tripwire-testcount 2>/dev/null \
          && info "ratchet: $REF -> $TC tests (.tripwire-testcount mis à jour — à committer)"
      elif [ "$TC" -lt "$REF" ]; then
        if [ "${TRIPWIRE_RATCHET_STRICT:-0}" = "1" ]; then
          fail "ratchet: $TC tests, référence $REF — des tests ont disparu (baisse assumée ? mettre à jour .tripwire-testcount dans un commit)"
          r=1
        else
          info "⚠ ratchet: $TC tests vs $REF attendus — des tests ont disparu ?"
        fi
      fi
    fi
  fi
  return "$r"
}

TW_POST_FULL+=(tw_ratchet)
