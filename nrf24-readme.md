# Documentation Récepteur NRF24L01

Ce document décrit comment configurer un récepteur (ex: un autre ESP32, Arduino, ou dongle USB custom) pour recevoir les données de la souris MaSe.

## 📡 Configuration Radio

Le récepteur doit être configuré avec **exactement** les mêmes paramètres que l'émetteur (la souris).

*   **Canal RF (Channel)** : `76` (2476 MHz)
*   **Débit (Data Rate)** : `1 Mbps`
*   **Puissance (PA Level)** : `0 dBm`
*   **CRC** : **Activé (16-bit / 2 bytes)**. (Standard pour la plupart des librairies).
*   **Auto-Ack** : Désactivé (`EN_AA = 0x00`).
*   **Adresse RX (Pipe 0)** : `{0xCE, 0xCE, 0xCE, 0xCE, 0xCE}`
*   **Taille du Payload** : `5 octets`

## 📦 Structure du Payload

La souris envoie un paquet de **5 octets** à chaque mouvement ou clic.

| Octet | Nom | Type | Description |
| :--- | :--- | :--- | :--- |
| 0 | **Type** | `uint8_t` | Toujours `0x01` pour un rapport de souris. |
| 1 | **Boutons** | `uint8_t` | Masque binaire des boutons (voir ci-dessous). |
| 2 | **Delta X** | `int8_t` | Déplacement relatif X (-127 à 127). |
| 3 | **Delta Y** | `int8_t` | Déplacement relatif Y (-127 à 127). |
| 4 | **Scroll** | `int8_t` | Déplacement molette (-127 à 127). |

### Masque des Boutons (Octet 1)
*   `Bit 0` : Clic Gauche
*   `Bit 1` : Clic Droit
*   `Bit 2` : Clic Milieu (Molette)
*   `Bit 3` : Bouton Précédent (Optionnel)
*   `Bit 4` : Bouton Suivant (Optionnel)

## 💻 Exemple de Code (Pseudo-code Arduino/C++)

Voici comment traiter les données côté récepteur :

```cpp
// Configuration (Setup)
radio.begin();
radio.setChannel(76);
radio.setDataRate(RF24_1MBPS);
radio.setAutoAck(false);
radio.setCRCLength(RF24_CRC_16); // CRC 16-bit activé
const uint8_t address[5] = {0xCE, 0xCE, 0xCE, 0xCE, 0xCE};
radio.openReadingPipe(1, address);
radio.startListening();

// Boucle (Loop)
if (radio.available()) {
    uint8_t payload[5];
    radio.read(&payload, sizeof(payload));

    if (payload[0] == 0x01) { // Vérifier que c'est bien la souris
        uint8_t buttons = payload[1];
        int8_t dx = (int8_t)payload[2];
        int8_t dy = (int8_t)payload[3];
        int8_t scroll = (int8_t)payload[4];

        // Action : Déplacer la souris sur le PC hôte
        // Ex: Mouse.move(dx, dy, scroll);
        // Ex: Mouse.set_buttons(buttons);
    }
}
```

## ⚠️ Notes Importantes

1.  **Latence** : Comme l'Auto-Ack est désactivé, il n'y a pas de retransmission. C'est idéal pour une souris (meilleure latence), car un paquet perdu est vite remplacé par le suivant.
2.  **Interférences** : Le canal 76 est au-dessus des canaux WiFi standard (généralement jusqu'à 13/14), ce qui limite les interférences.
3.  **Signé vs Non-signé** : Assurez-vous de caster les octets X, Y et Scroll en `int8_t` (signé) pour gérer les mouvements négatifs (gauche/haut).
