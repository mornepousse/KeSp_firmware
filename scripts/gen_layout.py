#!/usr/bin/env python3
"""Genere boards/niphar_layout.inc depuis le PCB du Niphargus.

    scripts/gen_layout.py [chemin/vers/niphar.kicad_pcb]

La geometrie 2D affichee par KeSp_controller etait dessinee a la main et ne
ressemblait pas a la carte : l'arc de pouces, en particulier, etait rendu
comme une rangee plate etalee sur toute la largeur alors que sur le PCB c'est
un cluster compact cote interieur. Ce script lit la verite dans le PCB :

  - position et orientation de chaque switch de la moitie gauche (feuille /s3/)
  - identite electrique (row, col) de chacun, en suivant les nets depuis le
    switch a travers la diode de matrice et les resistances serie jusqu'aux
    nets /s3/rowN et /s3/colN du module ESP32

puis emet la moitie gauche en unites de touche, et la droite par miroir.

Conventions :
  - 1 unite = 19,05 mm ; x,y = coin haut-gauche de la touche (comme le lit
    KeSp_controller, qui dessine a x*UNIT_PX avec une largeur w*UNIT_PX)
  - r positif = sens horaire a l'ecran (Slint : transform-rotation = r * 1deg).
    KiCad compte en sens trigo et les switches de cette moitie sont "droits"
    a 180 deg, d'ou r = 180 - rot_kicad.
  - moitie droite : meme PCB retourne. x' = 2*W + G - x - w, r' = -r,
    colonne keymap 13 - c (cf. half_col_to_keymap). W = largeur reelle de la
    moitie gauche, G = 1 unite d'ecart entre les deux.
"""
import re, sys, os, json
from collections import deque

UNIT = 19.05
GAP  = 1.0
HERE = os.path.dirname(os.path.abspath(__file__))
PCB  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, '..', '..', 'Niphargus', 'hardware', 'pcb', 'niphar.kicad_pcb')
OUT  = os.path.join(HERE, '..', 'boards', 'niphar_layout.inc')

# ---------------------------------------------------------------- lecture PCB
s = open(PCB).read()
fps = {}
for fp, b in re.findall(r'\n\t\(footprint\s+"([^"]+)"(.*?)(?=\n\t\(footprint\s+"|\Z)', s, re.S):
    r  = re.search(r'\(property "Reference" "([^"]+)"', b)
    sh = re.search(r'\(sheetname "([^"]*)"\)', b)
    at = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', b)
    if not r: continue
    pads = {p.group(1): p.group(2) for p in re.finditer(
        r'\(pad "([^"]*)"[^\n]*\n(?:(?!\(pad ")[\s\S])*?\(net "([^"]*)"\)', b) if p.group(2)}
    fps[r.group(1)] = dict(sheet=sh.group(1) if sh else '', x=float(at.group(1)),
                           y=float(at.group(2)), rot=float(at.group(3) or 0), pads=pads)
net_of = {}
for ref, d in fps.items():
    for n in d['pads'].values():
        net_of.setdefault(n, []).append(ref)

def traversable(ref):
    # R, D, et U1..U4 (des diodes malgre leur reference) : 2 bornes, on passe a travers
    return (ref[0] in 'RD' or re.fullmatch(r'U[1-4]', ref)) and len(fps[ref]['pads']) == 2

def resolve(net):
    seen, q = {net}, deque([(net, 0)])
    while q:
        n, depth = q.popleft()
        m = re.fullmatch(r'/s3/(row|col)(\d)', n)
        if m: return m.group(1), int(m.group(2))
        if depth >= 3 or n == 'GND': continue
        for ref in net_of.get(n, []):
            if traversable(ref):
                for n2 in fps[ref]['pads'].values():
                    if n2 not in seen:
                        seen.add(n2); q.append((n2, depth + 1))
    return None, None

keys = []
for ref, d in fps.items():
    if not ref.startswith('SW') or d['sheet'] != '/s3/': continue
    row = col = None
    for n in d['pads'].values():
        k, v = resolve(n)
        if k == 'row': row = v
        if k == 'col': col = v
    if row is None or col is None:
        sys.exit(f"{ref} : identite (row,col) non resolue")
    r = (180.0 - d['rot']) % 360.0
    if r > 180: r -= 360
    keys.append(dict(ref=ref, row=row, col=col, cx=d['x']/UNIT, cy=d['y']/UNIT, r=round(r, 1)))

# ---------------------------------------------------------------- normalisation
# coin haut-gauche = centre - 0.5 ; origine posee sur le min
x0 = min(k['cx'] for k in keys) - 0.5
y0 = min(k['cy'] for k in keys) - 0.5
for k in keys:
    k['x'] = round(k['cx'] - 0.5 - x0, 2)
    k['y'] = round(k['cy'] - 0.5 - y0, 2)
W = max(k['x'] + 1.0 for k in keys)

# ---------------------------------------------------------------- verifications
ids = [(k['row'], k['col']) for k in keys]
assert len(ids) == len(set(ids)), "doublon (row,col)"
assert len(keys) == 26, f"{len(keys)} touches, 26 attendues"
creux = [(r, c) for r in range(4) for c in range(7) if (r, c) not in set(ids)]
assert creux == [(2, 6), (3, 6)], f"creux inattendus : {creux}"

# ---------------------------------------------------------------- emission
def entry(row, col, x, y, r):
    e = f'{{\\"row\\":{row},\\"col\\":{col},\\"x\\":{x:.2f},\\"y\\":{y:.2f}'
    if abs(r) > 0.05: e += f',\\"r\\":{r:.1f}'
    return e + '}'

gauche = [entry(k['row'], k['col'], k['x'], k['y'], k['r'])
          for k in sorted(keys, key=lambda k: (k['row'], k['col']))]
droite = [entry(k['row'], 13 - k['col'], round(2*W + GAP - k['x'] - 1.0, 2), k['y'], -k['r'])
          for k in sorted(keys, key=lambda k: (k['row'], k['col']))]

lignes = [
'/*',
' * Niphargus — physical key layout, GENERE par scripts/gen_layout.py depuis',
' * hardware/pcb/niphar.kicad_pcb du depot Niphargus. NE PAS EDITER A LA MAIN :',
' * corriger le PCB ou le script, puis regenerer.',
' *',
' * Moitie gauche = feuille /s3/, 4 rows x 7 cols electriques, 26 touches ;',
' * (row2,col6) et (row3,col6) n\'existent pas. Chaque touche est positionnee',
' * individuellement avec sa rotation reelle : les colonnes 0-1 sont evasees de',
' * 10 deg, la colonne 2 de 5 deg, les autres droites ; l\'arc de pouces est un',
' * cluster compact cote interieur qui s\'evente de 0 a 45 deg. L\'ordre',
' * electrique des pouces (col 3,4,5,2,0,1 de l\'interieur vers l\'exterieur) ne',
' * suit pas l\'ordre physique — c\'est ce qui rendait l\'ancien dessin faux.',
' *',
' * Unites : 1 = 19,05 mm ; x,y = coin haut-gauche ; r > 0 = sens horaire.',
' * Requires: PRODUCT_NAME, MATRIX_ROWS, MATRIX_COLS, STR() defined.',
' */',
'#ifndef KEYMAP_COLS',
'#define KEYMAP_COLS MATRIX_COLS',
'#endif',
'',
f'/* Moitie droite : miroir de la gauche, x\' = 2W + G - x - w avec W = {W:.2f},',
f' * G = {GAP:.0f} ; rotations opposees ; la colonne physique c porte la colonne',
' * keymap 13 - c (cf. half_col_to_keymap). Ajoutee seulement sur le maitre,',
' * ou KEYMAP_COLS vaut 14. */',
'',
'const char board_layout_json[] =',
'"{"',
'  "\\"name\\":\\"" PRODUCT_NAME "\\","',
'  "\\"rows\\":" STR(MATRIX_ROWS) ","',
'  "\\"cols\\":" STR(KEYMAP_COLS) ","',
'  "\\"keys\\":["',
]
lignes += [f'    "{e},"' for e in gauche[:-1]] + [f'    "{gauche[-1]}"']
lignes += ['#if KEYMAP_COLS > MATRIX_COLS']
lignes += [f'    ",{droite[0]},"'] + [f'    "{e},"' for e in droite[1:-1]] + [f'    "{droite[-1]}"']
lignes += ['#endif', '  "],"', '  "\\"groups\\":[]"', '"}"', ';', '']
open(OUT, 'w').write('\n'.join(lignes))
print(f"{OUT} : {len(keys)} touches gauche, largeur {W:.2f} u, miroir a partir de x = {W + GAP/2:.2f}")
