#!/usr/bin/env python3
"""Draws a keymap (KeSp_controller export) on the keyboard's real geometry.

    scripts/render_keymap.py <layers.json> <output.png> [--diff <other.json>]

One image per non-empty layer. With --diff, cells that differ from the other
file are colored: green = modified/added, pink = cleared. This is the
brainstorming tool: the JSON is edited cell by cell, rendered, compared — the
proposal lives in the file, not in the head of whoever is drawing it.

Geometry: compiles boards/niphar_layout.inc via the C preprocessor (KEYMAP_COLS=14).
Keycodes: HID + families from main/input/key_definitions.h (MO/TO, K_MK, macros).
Rotation: r > 0 = clockwise, same as Slint in KeSp_controller.
"""
import sys, os, json, subprocess, tempfile, re
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')

def geometrie():
    src = f'''#include <stdio.h>
#define PRODUCT_NAME "Niphargus"
#define MATRIX_ROWS 4
#define MATRIX_COLS 7
#define KEYMAP_COLS 14
#define STR_(x) #x
#define STR(x) STR_(x)
#include "boards/niphar_layout.inc"
int main(void){{ printf("%s\\n", board_layout_json); return 0; }}
'''
    d = tempfile.mkdtemp()
    open(f'{d}/g.c', 'w').write(src)
    subprocess.run(['gcc', '-I', ROOT, '-o', f'{d}/g', f'{d}/g.c'], check=True)
    return json.loads(subprocess.run([f'{d}/g'], capture_output=True, text=True, check=True).stdout)['keys']

HID = {4:'A',5:'B',6:'C',7:'D',8:'E',9:'F',10:'G',11:'H',12:'I',13:'J',14:'K',15:'L',16:'M',17:'N',
 18:'O',19:'P',20:'Q',21:'R',22:'S',23:'T',24:'U',25:'V',26:'W',27:'X',28:'Y',29:'Z',
 30:'1',31:'2',32:'3',33:'4',34:'5',35:'6',36:'7',37:'8',38:'9',39:'0',
 40:'Enter',41:'Esc',42:'Backsp',43:'Tab',44:'Space',45:'-',46:'=',47:'[',48:']',49:'\\',
 51:';',52:"'",53:'`',54:',',55:'.',56:'/',57:'Caps',58:'F1',59:'F2',60:'F3',61:'F4',62:'F5',63:'F6',
 64:'F7',65:'F8',66:'F9',67:'F10',68:'F11',69:'F12',70:'PrtSc',73:'Ins',74:'Home',75:'PgUp',76:'Del',
 77:'End',78:'PgDn',79:'→',80:'←',81:'↓',82:'↑',224:'Ctrl',225:'Shift',226:'Alt',227:'GUI',
 228:'RCtrl',229:'RShift',230:'AltGr',231:'RGUI'}
SHIFTED = {30:'!',31:'@',32:'#',33:'$',34:'%',35:'^',36:'&',37:'*',38:'(',39:')',45:'_',46:'+',
 47:'{',48:'}',49:'|',51:':',52:'"',53:'~',54:'<',55:'>',56:'?'}

def nom(v, noms):
    if v == 0: return ''
    if v in HID: return HID[v]
    if 0x0100 <= v < 0x0B00: return 'MO(' + noms[(v >> 8) - 1].replace('LAYER ', 'L') + ')'
    if 0x0B00 <= v < 0x1500: return 'TO(' + noms[(v >> 8) - 0x0B].replace('LAYER ', 'L') + ')'
    if 0x1500 <= v < 0x2900: return f'M{(v >> 8) - 0x14}'
    if (v & 0xF000) == 0x8000:
        mod, kc = (v >> 8) & 0x0F, v & 0xFF
        if mod == 0x02 and kc in SHIFTED: return SHIFTED[kc]
        return f'{"CSAG"[[1,2,4,8].index(mod)] if mod in (1,2,4,8) else "?"}+{HID.get(kc, kc)}'
    return f'{v:04X}'

def rendre(chemin, sortie, ref=None):
    geo = geometrie()
    d = json.load(open(chemin)); noms = d['layer_names']; km = d['keymaps']
    dr = json.load(open(ref))['keymaps'] if ref else None
    couches = [i for i in range(len(km)) if any(v for row in km[i] for v in row)]
    S, M = 62, 26
    maxx = max(k['x'] + k.get('w', 1) for k in geo); maxy = max(k['y'] for k in geo) + 1
    PW = int(maxx * S) + 2 * M; PH = int(maxy * S) + 34 + 16
    esc = lambda t: t.replace('&', '&amp;').replace('<', '&lt;')
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{PW}" height="{PH * len(couches) + 20}">',
         f'<rect width="{PW}" height="{PH * len(couches) + 20}" fill="#fbfbfb"/>']
    for n, i in enumerate(couches):
        oy = n * PH + 10
        o.append(f'<text x="{M}" y="{oy + 18}" font-family="sans-serif" font-size="20" font-weight="bold" fill="#0f172a">{esc(noms[i])}</text>')
        if n: o.append(f'<line x1="{M}" y1="{oy - 6}" x2="{PW - M}" y2="{oy - 6}" stroke="#cbd5e1"/>')
        for k in geo:
            r_, c, x, y, rot, w = k['row'], k['col'], k['x'], k['y'], k.get('r', 0), k.get('w', 1)
            v = km[i][r_][c]; lab = nom(v, noms)
            px, py, pw, ph = x * S + M, y * S + oy + 34, w * S, S; cx, cy = px + pw / 2, py + ph / 2
            chg = dr is not None and dr[i][r_][c] != v
            if chg and v: fill = '#bbf7d0'
            elif chg: fill = '#fee2e2'
            elif lab.startswith(('MO(', 'TO(')): fill = '#fdba74'
            elif r_ == 3 and lab: fill = '#fecaca'
            elif lab: fill = '#ffffff'
            else: fill = '#f1f5f9'
            o.append(f'<g transform="rotate({rot} {cx} {cy})"><rect x="{px + 2}" y="{py + 2}" width="{pw - 4}" height="{ph - 4}" rx="7" fill="{fill}" stroke="#334155" stroke-width="1.8"/>')
            if chg and not v and dr[i][r_][c]:
                lab, colr = nom(dr[i][r_][c], noms), '#9ca3af'
            else:
                colr = '#0f172a'
            fs = 18 if len(lab) <= 1 else (15 if len(lab) <= 2 else (13 if len(lab) <= 5 else 10))
            o.append(f'<text x="{cx}" y="{cy + 6}" font-family="sans-serif" font-size="{fs}" text-anchor="middle" fill="{colr}">{esc(lab)}</text></g>')
    o.append('</svg>')
    svg = sortie.rsplit('.', 1)[0] + '.svg'
    open(svg, 'w').write('\n'.join(o))
    subprocess.run(['inkscape', '--export-type=png', f'--export-filename={sortie}', '--export-dpi=96', svg],
                   check=True, capture_output=True)
    print(f'{sortie}: {len(couches)} layer(s) — {", ".join(noms[i] for i in couches)}')

if __name__ == '__main__':
    a = sys.argv[1:]
    ref = a[a.index('--diff') + 1] if '--diff' in a else None
    a = [x for x in a if x != '--diff' and x != ref]
    rendre(a[0], a[1], ref)
