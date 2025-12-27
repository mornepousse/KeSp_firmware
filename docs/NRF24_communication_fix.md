# Correction des problèmes de communication NRF24 Souris ↔ Clavier

## Architecture du système

```
┌─────────────────┐         Radio 2.4GHz        ┌─────────────────┐
│     SOURIS      │  ─────────────────────────▶ │     CLAVIER     │
│   (Émetteur)    │       Canal 76, 1Mbps       │   (Récepteur)   │
│   NRF24L01+     │       Adresse: CE×5         │   NRF24L01+     │
└─────────────────┘                             └─────────────────┘
                                                        │
                                                        ▼
                                                ┌─────────────────┐
                                                │   USB HID PC    │
                                                └─────────────────┘
```

## Configuration Radio

| Paramètre | Valeur |
|-----------|--------|
| Canal RF | 76 (2476 MHz) |
| Débit | 1 Mbps |
| Puissance | 0 dBm |
| CRC | 16-bit activé |
| Auto-Ack | Désactivé |
| Adresse (5 octets) | `0xCE:CE:CE:CE:CE` |
| Taille Payload | 5 octets |

## Format du Payload Souris

| Octet | Nom | Type | Description |
|-------|-----|------|-------------|
| 0 | Type | `uint8_t` | `0x01` = rapport souris |
| 1 | Boutons | `uint8_t` | Bit 0: Gauche, Bit 1: Droit, Bit 2: Milieu |
| 2 | Delta X | `int8_t` | Mouvement horizontal (-127 à +127) |
| 3 | Delta Y | `int8_t` | Mouvement vertical (-127 à +127) |
| 4 | Scroll | `int8_t` | Molette (-127 à +127) |

---

## Problème 1 : Bug SPI dans la souris (Émetteur)

### Fichier concerné
`/MaSe_CoDe/main/nrf24.c`

### Symptôme
La souris émet sur une adresse aléatoire au lieu de `0xCE:CE:CE:CE:CE`.

### Cause
La fonction `nrf24_write_register_multi()` faisait **2 transactions SPI séparées** :

```c
// ❌ ANCIEN CODE CASSÉ
cs_select();
spi_device_transmit(nrf_spi, &t_cmd);   // Transaction 1: commande
spi_device_transmit(nrf_spi, &t_data);  // Transaction 2: données
cs_deselect();
```

Le NRF24L01 exige que **commande + données soient envoyées dans UNE SEULE transaction SPI continue**. Entre les 2 transactions, le timing est cassé et le module ignore les données.

### Solution
Combiner commande et données dans un seul buffer :

```c
// ✅ NOUVEAU CODE
uint8_t *tx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
tx_buf[0] = NRF_W_REGISTER | (reg & 0x1F);  // Commande
memcpy(&tx_buf[1], data, len);               // Données

cs_select();
spi_transaction_t t = {
    .length = (len + 1) * 8,
    .tx_buffer = tx_buf,  // Tout dans un seul buffer
};
spi_device_transmit(nrf_spi, &t);  // UNE seule transaction
cs_deselect();
free(tx_buf);
```

### Fonctions corrigées
- `nrf24_write_register_multi()` - écriture d'adresse
- `nrf24_send()` - envoi de payload
- `nrf24_read_register()` - buffer TX de 2 octets

---

## Problème 2 : Lecture du FIFO au lieu du flag RX_DR (Récepteur)

### Fichier concerné
`/Mae_Keyboard_Code/main/comm/nrf24_receiver.c`

### Symptôme
Les logs montraient `RPD=1` (signal détecté) et `FIFO=0x12` (FIFO plein) mais `RX=0` (aucun paquet traité).

### Cause
Le code attendait le flag `RX_DR` (bit 6 du registre STATUS) qui n'était jamais activé :

```c
// ❌ ANCIEN CODE
if (status & 0x40) {  // RX_DR jamais activé !
    nrf_read_payload(payload, 5);
}
```

Analyse de `FIFO_STATUS = 0x12` :
```
0x12 = 0001 0010 en binaire
       ││││ ││││
       ││││ ││└┴─ TX_EMPTY/TX_FULL (ignoré)
       ││││ └──── (réservé)
       │││└────── RX_FULL = 1 → FIFO RX PLEIN !
       ││└─────── RX_EMPTY = 0 → Il y a des données !
       └┴──────── (réservé)
```

### Solution
Lire basé sur le FIFO status au lieu du flag RX_DR :

```c
// ✅ NOUVEAU CODE
uint8_t fifo = nrf_read_reg(FIFO_STATUS);
while (!(fifo & 0x01)) {  // Tant que RX_EMPTY = 0 (il y a des données)
    nrf_read_payload(payload, 5);
    nrf_write_reg(STATUS, 0x70);  // Clear tous les IRQ flags
    
    if (payload[0] == 0x01) {
        send_mouse_report(payload[1], (int8_t)payload[2], 
                         (int8_t)payload[3], (int8_t)payload[4]);
    }
    
    fifo = nrf_read_reg(FIFO_STATUS);  // Relire pour la boucle
}
```

---

## Problème 3 : Descripteur HID USB incomplet (Clavier)

### Fichiers concernés
- `/Mae_Keyboard_Code/main/comm/usb_descriptors.c`
- `/Mae_Keyboard_Code/main/input/keyboard_manager.c`

### Symptôme
Quand la souris bougeait, des caractères aléatoires étaient tapés sur le PC.

### Cause
1. Le descripteur HID ne contenait **que le clavier, pas de souris**
2. Les reports étaient envoyés avec Report ID = 0

```c
// ❌ ANCIEN CODE
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()  // Pas de souris !
};

tud_hid_mouse_report(0, buttons, x, y, wheel, 0);  // Report ID 0
```

Windows interprétait les données souris comme des données clavier.

### Solution
Ajouter le descripteur souris avec des Report IDs distincts :

```c
// ✅ NOUVEAU CODE - usb_descriptors.c
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE = 2,
};

const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};
```

```c
// ✅ NOUVEAU CODE - keyboard_manager.c
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2

tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycodes);
tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
```

---

## Résumé des fichiers modifiés

| Projet | Fichier | Modification |
|--------|---------|--------------|
| Souris | `main/nrf24.c` | Transaction SPI unique pour multi-octets |
| Clavier | `main/comm/nrf24_receiver.c` | Lecture FIFO au lieu de RX_DR |
| Clavier | `main/comm/usb_descriptors.c` | Ajout descripteur souris + Report IDs |
| Clavier | `main/input/keyboard_manager.c` | Utilisation des Report IDs corrects |

---

## Validation

### Logs attendus - Souris
```
I (xxx) NRF24: NRF24L01 Detected successfully.
I (xxx) NRF24: TX Addr set to: CE:CE:CE:CE:CE
I (xxx) NRF24: Power Up TX - Config: 0x0E
I (xxx) Main: NRF24 TX: 125 pkts/s | Status: 0x2E
```

### Logs attendus - Clavier
```
I (xxx) NRF24: NRF24 Initialized (Fixed Config: CH76, 1Mbps, CRC16)
I (xxx) NRF24: DATA RECEIVED! [01 00 05 F3 00]
I (xxx) NRF24: DATA RECEIVED! [01 00 08 F1 00]
I (xxx) NRF24: CH76 DIAG | RPD(Carrier): 1 | St: 0x40 | FIFO: 0x11 | CFG: 0x0F | RX: 48
```

### Interprétation d'un payload
```
[01 00 05 F3 00]
 │  │  │  │  └─ Scroll = 0
 │  │  │  └─ Delta Y = 0xF3 = -13 (mouvement vers le haut)
 │  │  └─ Delta X = 0x05 = +5 (mouvement vers la droite)
 │  └─ Boutons = 0x00 (aucun clic)
 └─ Type = 0x01 (rapport souris)
```

---

## Dépannage

| Symptôme | Cause probable | Solution |
|----------|----------------|----------|
| `RPD=0` constamment | Souris éteinte ou mauvaise adresse | Vérifier alimentation souris et logs TX |
| `FIFO=0x11` (vide) | Adresses différentes | Vérifier que TX et RX ont `CE:CE:CE:CE:CE` |
| Caractères tapés au lieu de mouvement | Report ID incorrect | Vérifier `REPORT_ID_MOUSE = 2` |
| Mouvement saccadé | Trop de latence | Réduire le délai de boucle à 1-5ms |

---

*Document créé le 19 décembre 2024*
