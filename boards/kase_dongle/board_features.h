#ifndef BOARD_FEATURES_H
#define BOARD_FEATURES_H

/* Static capability flags for kase_dongle board.
 * These mirror the Kconfig flags but are visible to C code (Kconfig flags
 * gate CMake compilation; these gate runtime behavior in main.c). */

#define BOARD_HAS_LOCAL_MATRIX    0   /* No local matrix scan */
#define BOARD_HAS_DISPLAY         0   /* No display */
#define BOARD_HAS_BLE             0   /* No Bluetooth */
#define BOARD_HAS_TAMA            0   /* No virtual pet */
#define BOARD_HAS_RF_RX           1   /* 2x NRF24L01+ on shared SPI */
#define BOARD_HAS_ESPNOW          1   /* ESP-NOW cold path (OTA, config) */
#define BOARD_HAS_USB_HID         1   /* USB HID output (NKRO + mouse + consumer) */
#define BOARD_HAS_USB_CDC         1   /* CDC binary protocol */

#endif /* BOARD_FEATURES_H */
