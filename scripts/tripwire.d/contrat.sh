#!/usr/bin/env bash
# Brique tripwire : contrat de comportements (COMPORTEMENTS.md, .tripwire-nongardes)
# Sourcee par scripts/check.sh si presente. La retirer = supprimer ce fichier.
# Ne rien appeler ici : la brique s'enregistre, le noyau appelle.

# ---- Contrat de comportements : une garde ne disparaît pas en silence ----
# COMPORTEMENTS.md (committé) : puces taguées [test:X] (X doit apparaître dans un
# fichier de test), [smoke:X] (note manuelle, jamais vérifiée ici) ou [NON GARDÉ]
# (l'aveu, compté et ratcheté dans .tripwire-nongardes comme le ratchet de
# tests). Toute autre ligne est de la prose. Absent -> inerte.
check_comportements() {
  [ -f COMPORTEMENTS.md ] || return 0
  local rc=0 n=0 ng=0 line tag arg testfiles
  testfiles="$( { git ls-files; git ls-files -o --exclude-standard; } 2>/dev/null | grep -E "${TEST_GREP:-^\$}" | sort -u )"
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1)); line="${line%$'\r'}"
    case "$line" in *'- ['*']'*) ;; *) continue ;; esac
    tag="${line#*- [}"; tag="${tag%%]*}"
    case "$tag" in
      'NON GARDÉ') ng=$((ng + 1)) ;;
      test:*)
        arg="${tag#test:}"
        if [ -z "$TEST_GREP" ]; then
          fail "comportement ligne $n : garde [test:$arg] invérifiable — TEST_GREP est vide dans check.sh"; rc=1
        elif ! printf '%s\n' "$testfiles" | xargs -d '\n' grep -qF -- "$arg" 2>/dev/null; then
          fail "comportement ligne $n a perdu sa garde : « $arg » n'apparaît dans aucun fichier de test"
          echo "  → rétablir le test, ou passer la ligne en [NON GARDÉ] si l'aveu est assumé (il est compté)." >&2
          rc=1
        fi ;;
      smoke:*) ;;   # note de vérification manuelle (release) : jamais vérifiée ici
      NON*|*:*)
        fail "comportement ligne $n : tag inconnu [$tag] — attendu [test:X], [smoke:X] ou [NON GARDÉ]"; rc=1 ;;
      *) ;;   # lien markdown ou prose entre crochets : pas un tag
    esac
  done < COMPORTEMENTS.md
  # Ratchet des NON GARDÉ : même mécanique que .tripwire-testcount.
  local REF; REF="$(cat .tripwire-nongardes 2>/dev/null | tr -d '[:space:]')"
  case "$REF" in ''|*[!0-9]*) REF="" ;; esac
  if [ -z "$REF" ]; then
    printf '%s\n' "$ng" > .tripwire-nongardes 2>/dev/null \
      && info "contrat: $ng comportement(s) NON GARDÉ — référence initialisée (.tripwire-nongardes, à committer)"
  elif [ "$ng" -lt "$REF" ]; then
    printf '%s\n' "$ng" > .tripwire-nongardes 2>/dev/null \
      && info "contrat: NON GARDÉ $REF -> $ng (.tripwire-nongardes mis à jour — à committer)"
  elif [ "$ng" -gt "$REF" ]; then
    if [ "${TRIPWIRE_RATCHET_STRICT:-0}" = "1" ]; then
      fail "contrat: $ng comportements NON GARDÉ, référence $REF — le non-gardé a augmenté (assumé ? monter .tripwire-nongardes dans un commit)"
      rc=1
    else
      info "⚠ contrat: $ng comportements NON GARDÉ vs $REF — un de plus sans garde"
    fi
  fi
  return "$rc"
}

TW_PRE_FAST+=(check_comportements)
