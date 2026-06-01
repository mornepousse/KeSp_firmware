# KaSe dongle — guide d'integration soft

Ce document s'adresse a l'equipe KeSp_controller (le remapping software).
Il decrit comment detecter, dialoguer et afficher l'etat d'un dongle KaSe
en USB, par opposition a un clavier autonome (V1/V2/V2D).

> Pre-requis : avoir lu `CDC_BINARY_PROTOCOL.md` pour le framing KS/KR + CRC-8.
> Toutes les commandes ici sont des `KS_CMD_*` standards.

---

## 1. Detection du role (dongle vs clavier autonome)

Le firmware dongle et le firmware clavier exposent **le meme device USB** :
HID composite (keyboard + mouse) + CDC ACM. Le VID/PID ne change pas. Pour
distinguer les deux, le soft doit envoyer `KS_CMD_FEATURES` (0x02) au boot et
chercher le tag `RF_DONGLE` dans la chaine retournee.

```
KS [02] []  → KR [02] OK "MT,LT,LM,...,MATRIX_TEST,RF_DONGLE,RF_STATUS,RF_PAIR,BATTERY"
```

| Tag presence | Role detecte                |
|--------------|-----------------------------|
| `RF_DONGLE`  | Dongle sans fil (split)     |
| absent       | Clavier autonome (V1/V2/V2D)|

Sur un clavier autonome, les commandes `RF_*` et `BATTERY` repondent
`ERR_UNKNOWN` (status 0x01) — gerer comme tag absent.

**Recommandation** : cacher `paired_count`, signal_bars et batterie de l'UI
quand le tag est absent, pour ne pas afficher des champs vides sur un V2.

---

## 2. Cycle de vie typique

```
┌─ Le soft demarre ────────────────────────────────────────────┐
│ 1. Ouvre /dev/ttyACM<N> (CDC ACM)                            │
│ 2. envoie KS_CMD_PING → KR_OK pour verifier le lien CDC      │
│ 3. envoie KS_CMD_VERSION → version dongle (ex: v3.8.0-...)   │
│ 4. envoie KS_CMD_FEATURES → detecte tag RF_DONGLE            │
│ 5. si dongle → envoie KS_CMD_RF_PAIR_LIST → MACs paires      │
│ 6. lance la pompe de polling RF_STATUS (toutes les 1–2 s)    │
│ 7. lance la pompe de polling BATTERY (toutes les 5–10 s)     │
└──────────────────────────────────────────────────────────────┘
```

Les commandes "lourdes" (keymap, macros, layout) restent identiques entre
dongle et clavier autonome — c'est volontaire : le dongle stocke le keymap et
fait tout le decoding HID. Les moities n'envoient que les bytes de matrice.

---

## 3. Affichage du lien radio (polling RF_STATUS)

`KS_CMD_RF_STATUS` (0xB3) retourne 27 octets, idempotent, sans effet de bord.
Cadence recommandee : **1–2 Hz** dans l'UI (la ressource est gratuite, mais
pas la peine de saturer le CDC).

### Bytes de sortie (rappel)

```
[0]    flags         bit0=link_L_up, bit1=link_R_up
[1]    sig_L         0..255 (0 = down)
[2]    sig_R         0..255 (0 = down)
[3..6] hb_age_L_ms   u32 LE
[7..10] hb_age_R_ms  u32 LE
[11..14] pkt_rx_L    u32 LE
[15..18] pkt_rx_R    u32 LE
[19..22] pkt_dup_L   u32 LE
[23..26] pkt_dup_R   u32 LE
```

### Mapping signal → barres

Le firmware encode deja la qualite combinee (heartbeat age + retry count)
dans `sig_<side>` via `rf_signal_q255()`. Le soft ne fait que mapper :

```python
def bars(sig: int) -> int:
    if sig >= 200: return 4
    if sig >= 140: return 3
    if sig >=  80: return 2
    if sig >=  30: return 1
    return 0  # lien perdu, afficher icone X
```

### Etat des trois moities possibles

| Moitie         | `link_up` | `pkt_rx` | Interpretation                  |
|----------------|-----------|----------|---------------------------------|
| Non vue        | 0         | 0        | Jamais connectee (pas paire ou eteinte) |
| Liee, sans contact | 0     | > 0      | Connue mais lien casse (pile vide, hors portee) |
| Active         | 1         | > 0      | OK, afficher `bars(sig)`        |

### Compteurs `pkt_dup_*`

Les duplicats sont les paquets retransmis par la moitie qui ont la meme `seq`
qu'un paquet deja accepte. C'est normal et indique la sante du lien : taux
de duplicats stable = lien stable ; pic = perturbation radio. Le soft peut
afficher ce ratio dans un panneau "diagnostic" mais ce n'est pas vital.

---

## 4. Workflow de pairing

Le pairing est une operation utilisateur : appui sur un bouton "+ Half" dans
le soft, qui declenche cette sequence.

```
soft                                  dongle              half
 │                                       │                  │
 │   KS_CMD_RF_PAIR_START [reset=0]      │                  │
 ├──────────────────────────────────────>│                  │
 │   KR OK [set_id_hi,set_id_lo,paired]  │                  │
 │<──────────────────────────────────────┤                  │
 │                                       │  (radio L sur    │
 │                                       │   rendezvous,    │
 │                                       │   30s window)    │
 │                                       │                  │
 │                                       │<- rf_pair_req ───┤
 │                                       ├-- rf_pair_ack --→│
 │                                       │                  │
 │   (poll RF_PAIR_LIST every 2 s)       │                  │
 ├──────────────────────────────────────>│                  │
 │   KR OK [paired=1, mac_L=AA..., mac_R=00...]             │
 │<──────────────────────────────────────┤                  │
```

**UI suggeree** :
1. Bouton "Pair half" affiche un compte-a-rebours de 30 s
2. Toutes les 2 s, poller `RF_PAIR_LIST` et comparer `paired_count` au precedent
3. Si `paired_count` augmente : afficher "Half paired !" et exit
4. Si timeout : afficher "Timeout — verifier que la moitie est en mode pairing"

**`reset = 1` vs `reset = 0`** :
- `reset = 0` (par defaut) : ajoute aux paires existantes (max 2)
- `reset = 1` : equivalent a `RF_PAIR_RESET` puis `PAIR_START` — utile pour
  re-coupler from scratch (ex: nouvelle paire de moities)

---

## 5. Affichage de la batterie

`KS_CMD_BATTERY` (0xB6) retourne 14 octets : 7 par moitie. Cadence
recommandee : **5–10 s** dans l'UI (les moities n'envoient `EN_INFO_BATTERY`
que quand la valeur change, donc poller plus vite ne sert a rien).

```
slot 0 (LEFT) :  [batt_dV][soc_pct][charging][age_ms u32 LE]
slot 1 (RIGHT) : [batt_dV][soc_pct][charging][age_ms u32 LE]
```

### Valeurs sentinelles

| Champ      | `0xFF` / `0xFFFFFFFF` signifie                  |
|------------|-------------------------------------------------|
| `batt_dV`  | jamais recu de la moitie                        |
| `soc_pct`  | SoC inconnu (BMS pas branche / firmware ancien) |
| `charging` | etat inconnu                                    |
| `age_ms`   | aucun sample recu depuis le boot du dongle      |

### Regles d'affichage

```python
def render_battery(rec):
    dv, soc, chg, age = rec
    if dv == 0xFF or age == 0xFFFFFFFF:
        return "—"             # pas de telemetrie
    label = f"{soc}%"          # ou f"{dv/10:.1f} V"
    if chg == 1:
        label = "⚡ " + label
    if age > 60_000:           # plus de 60 s
        label = "(?) " + label  # potentiellement obsolete
    return label
```

---

## 6. Compatibilite et evolution

### Garanties

- Les IDs de commandes ne sont **jamais** reassignes. Un futur firmware peut
  ajouter `KS_CMD_XXX` sur un ID libre mais ne renumerotera pas les existants.
- Le tag `RF_DONGLE` reste le marqueur officiel du role.
- La taille des reponses RF_STATUS/RF_PAIR_LIST/BATTERY est **fixe** et ne
  changera pas. Toute extension future passera par un nouvel ID.

### Forward compatibility

Si le firmware ajoute des bits dans `RF_STATUS.flags` ou des champs apres
`pkt_dup_right`, **le soft doit ignorer les bits/octets inconnus** et utiliser
uniquement `len` de l'en-tete KR. Pas de hash sur le contenu.

### Reserve

Les IDs libres autour des commandes RF :
- `0xB7..0xBF` : reserves pour futures diagnostics dongle/wireless
- `0x96..0x9F` : reserves pour features generiques

---

## 7. Erreurs et reconnexion

| Status      | Quand                                       | Action coter soft                       |
|-------------|---------------------------------------------|-----------------------------------------|
| `OK` 0x00   | Tout va bien                                | —                                       |
| `ERR_BUSY` 0x05 | `RF_PAIR_START` quand fenetre deja ouverte | Backoff 2 s, retenter                  |
| `ERR_UNKNOWN` 0x01 | Commande envoyee a un clavier autonome | Verifier `RF_DONGLE` dans FEATURES     |
| `ERR_CRC` 0x02 | Mauvais CRC8 a la reception                | Reverifier le calcul CRC8 (poly 0x07)  |

**Deconnexion CDC** : si le `/dev/ttyACM<N>` disparait (dongle debranche
ou reboot), le soft doit (1) detecter via SIGIO/poll, (2) tenter de reouvrir
periodiquement (~1 s), (3) re-jouer le workflow de section 2 a la reconnexion
— l'etat du dongle (paires, layer, keymap) est entierement persiste en NVS,
rien a "restaurer".

---

## 8. Annexe : exemples de framing brut

Tous les exemples utilisent le CRC-8/MAXIM (polynome 0x31, init 0x00, no
reflection) calcule uniquement sur le **payload** (pas sur cmd_id ni len).
Un payload vide donne CRC = 0x00.

### Polling RF_STATUS

```
TX (soft → dongle) :
  4B 53      magic 'KS'
  B3         cmd RF_STATUS
  00 00      len = 0
  <crc8>     CRC sur [B3, 00, 00]

RX (dongle → soft) :
  4B 52      magic 'KR'
  B3         cmd RF_STATUS
  00         status OK
  1B 00      len = 27
  03         flags = link_L + link_R up
  C8 D2      sig_L=200 sig_R=210
  E8 03 00 00  hb_age_L = 1000 ms
  D0 07 00 00  hb_age_R = 2000 ms
  ... 16 bytes restants ...
  <crc8>
```

### Lecture pairing

```
TX : 4B 53  B4  00 00  <crc>
RX : 4B 52  B4  00  0D 00
     02                                       paired_count = 2
     AA BB CC DD EE FF                        mac_left
     11 22 33 44 55 66                        mac_right
     <crc>
```

### Demarrage pairing avec reset

```
TX : 4B 53  B2  01 00 01 <crc>     (reset = 1)
RX : 4B 52  B2  00  03 00 1A 2F 00 <crc>
     set_id = 0x1A2F, paired_count = 0
```

---

## 9. Annexe : reference rapide des IDs

| ID    | Nom               | Direction | Taille rep. | Frequence soft  |
|-------|-------------------|-----------|-------------|-----------------|
| 0x01  | VERSION           | get       | variable    | au connect      |
| 0x02  | FEATURES          | get       | variable    | au connect      |
| 0x04  | PING              | get       | 0           | heartbeat lent  |
| 0xB2  | RF_PAIR_START     | action    | 3 bytes     | sur action user |
| 0xB3  | RF_STATUS         | get       | 27 bytes    | 1–2 Hz          |
| 0xB4  | RF_PAIR_LIST      | get       | 13 bytes    | 0.5 Hz pendant pairing, sinon a la demande |
| 0xB5  | RF_PAIR_RESET     | action    | 1 byte      | sur action user |
| 0xB6  | BATTERY           | get       | 14 bytes    | 0.1–0.2 Hz      |

Pour la liste complete (keymap, macros, stats, etc.), voir
`CDC_BINARY_PROTOCOL.md`.
