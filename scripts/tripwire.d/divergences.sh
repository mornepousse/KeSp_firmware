#!/usr/bin/env bash
# Brique tripwire : divergences declarees (.tripwire-divergences)
# Sourcee par scripts/check.sh si presente. La retirer = supprimer ce fichier.
# Ne rien appeler ici : la brique s'enregistre, le noyau appelle.

# ---- Divergences déclarées : un écart assumé ne disparaît pas en silence ----
# .tripwire-divergences (committé), TSV : fichier<TAB>motif<TAB>pourquoi
# Absent/vide -> inerte. Motif comparé en chaîne littérale, jamais en regex.
# NOTE: cette fonction est dupliquée entre skills/init/templates/check.sh.tmpl
# et scripts/check.sh (dogfood). Duplication assumée : un check.sh scaffoldé est
# copié dans d'autres repos, il doit être autonome et ne rien sourcer du plugin.
# Toute correction ici se reporte à l'identique dans l'autre fichier.
check_divergences() {
  [ -f .tripwire-divergences ] || return 0
  if [ ! -r .tripwire-divergences ]; then
    fail "divergences : .tripwire-divergences illisible (droits ?) — une fiche illisible n'est pas une fiche vide"
    return 1
  fi
  local rc=0 n=0 line f rest m w
  # Découpage explicite : la tabulation est un caractère IFS-whitespace, un
  # `IFS=$'	' read` fusionnerait les tabs consécutives et décalerait les champs.
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    line="${line%$'\r'}"                                     # fiche en CRLF
    case "$line" in ''|'#'*) continue ;; esac
    f="${line%%$'	'*}"
    rest="${line#*$'	'}"; [ "$rest" = "$line" ] && rest=""  # aucune tabulation
    m="${rest%%$'	'*}"
    w="${rest#*$'	'}"; [ "$w" = "$rest" ] && w=""
    if [ -z "$f" ] || [ -z "$m" ]; then
      fail "divergence ligne $n : ligne malformée (attendu: fichier<TAB>motif<TAB>pourquoi)"
      rc=1; continue
    fi
    if [ ! -f "$f" ]; then
      fail "divergence perdue : $f n'existe plus (motif « $m »)"
      [ -n "$w" ] && echo "  motif déclaré : $w" >&2
      echo "  → rétablir la divergence, ou retirer sa ligne de .tripwire-divergences" >&2
      echo "    si l'abandon est voulu (le retrait part dans le diff, il sera vu en review)." >&2
      rc=1; continue
    fi
    if ! grep -qF -- "$m" "$f"; then
      fail "divergence perdue : $f ne contient plus « $m »"
      [ -n "$w" ] && echo "  motif déclaré : $w" >&2
      echo "  → rétablir la divergence, ou retirer sa ligne de .tripwire-divergences" >&2
      echo "    si l'abandon est voulu (le retrait part dans le diff, il sera vu en review)." >&2
      rc=1
    fi
  done < .tripwire-divergences
  return "$rc"
}

TW_PRE_FAST+=(check_divergences)
