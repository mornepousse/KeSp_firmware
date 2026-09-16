# Always-compiled core sources
set(srcs
    "main.c"
    "sys/cpu_time.c"
    "sys/nvs_utils.c"
    "app/dfu_manager.c"
    "app/littlefs_manager.c"

    # USB (always present : HID + CDC)
    "comm/usb/usb_hid.c"
    "comm/hid_transport.c"
    "comm/cdc/cdc_acm_com.c"
    "comm/cdc/cdc_ota.c"
    "comm/cdc/cdc_binary_protocol.c"
    "comm/cdc/cdc_binary_cmds.c"
    "comm/cdc/ks_monitor.c"

    # Config bridge: dongle↔smart-keyboard KS/KR tunnelling over ESP-NOW.
    # Pure chunk/predicate logic compiles on all boards; the ESP-NOW networking
    # functions are gated inside the file (dongle role / wireless keyboard).
    "comm/rf/cfg_bridge.c"

    # Security confirmation gate — compiled for all roles (key_processor calls it)
    "security/sec_confirm.c"

)

# Keymap et layout de la carte — sauf sur le dongle.
#
# Ils décrivaient une matrice 5×14 et une keymap de 70 touches sur un appareil
# qui n'a aucun interrupteur : le vestige de l'architecture A. Le dongle ne
# compile plus le moteur d'entrée ni le bloc CLAVIER du protocole CDC, donc plus
# personne n'y référence keymaps[] ni default_layout_names[].
# La souris est exclue pour la meme raison que le dongle : elle n'a aucun
# interrupteur en matrice. Ses trois clics sont lus individuellement par paires
# NO+NC, et boards/conchodytes/board.h ne declare donc ni ROWSn ni COLSn.
if(NOT CONFIG_KASE_NO_KEYMAP_ENGINE)
    list(APPEND srcs
        "${BOARD_DIR}/board_keymap.c"
        "${BOARD_DIR}/board_layout.c"
    )
endif()

# Moteur d'entrée — PAS sur le dongle.
#
# Il y était compilé du temps de l'architecture A, quand le dongle fusionnait les
# demi-matrices des anciennes moitiés et appliquait la keymap. Le Niphargus lui
# envoie du HID déjà fini (docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md),
# donc le dongle n'a plus rien à calculer — et surtout, l'exclure d'ici rend
# structurellement impossible l'existence de deux moteurs keymap dans le système,
# question qui s'est reposée trois fois en deux mois.
if(NOT CONFIG_KASE_NO_KEYMAP_ENGINE)
    list(APPEND srcs
        "input/key_processor.c"
        "input/tap_hold.c"
        "input/tap_dance.c"
        "input/combo.c"
        "input/leader.c"
        "input/key_features.c"
        "input/hid_report.c"
        "input/keymap.c"
        "input/key_stats.c"
        "input/keyboard_actions.c"
    )
endif()

# Local matrix scan + keyboard task — keyboard role only
if(CONFIG_KASE_HAS_LOCAL_MATRIX)
    list(APPEND srcs
        "input/matrix_scan.c"
        "input/keyboard_task.c"
    )
endif()

# Display backend + status display — keyboard role only
if(CONFIG_KASE_HAS_DISPLAY)
    if(DISPLAY_BACKEND STREQUAL "memlcd")
        # Sharp memory-LCD des moities Niphargus (portrait 68x160, SPI partage nRF24).
        list(APPEND srcs
            "display/memlcd/memlcd_panel.c"
            "display/memlcd/memlcd_backend.c"
            "display/assets/img_niphargus_60.c"
        )
    elseif(DISPLAY_BACKEND STREQUAL "round")
        list(APPEND srcs
            "display/round/spi_round_display.c"
            "display/round/round_ui.c"
            "display/round/round_backend.c"
        )
    else()
        list(APPEND srcs
            "display/oled/i2c_oled_display.c"
            "display/oled/oled_backend.c"
            "display/oled/oled_nav.c"
            "display/oled/oled_stats.c"
            "display/oled/oled_kpm.c"
            "display/oled/oled_screens.c"
            "display/oled/screens/screen_splash.c"
            "display/oled/screens/screen_home.c"
            "display/oled/screens/screen_stats.c"
        )
    endif()
    list(APPEND srcs
        "display/status_display.c"
        "display/assets/img_usb.c"
        "display/assets/img_signal.c"
        "display/assets/img_bluetooth.c"
        "led/led_strip_anim.c"
        "led/led_curve.c"
    )
endif()

# Bouchons d'affichage : rôle clavier SANS écran (Niphargus gauche). Les appels
# status_display_* sont dispersés dans le chemin de frappe (matrix_scan.c,
# keyboard_actions.c) ; les bouchonner ici évite d'y semer des #if permanents.
if(CONFIG_KASE_DEVICE_ROLE_KEYBOARD AND NOT CONFIG_KASE_HAS_DISPLAY)
    list(APPEND srcs "display/status_display_stubs.c")
endif()

# BLE HID — keyboard role only
if(CONFIG_KASE_HAS_BLE)
    list(APPEND srcs
        "comm/ble/esp_hidd_prf_api.c"
        "comm/ble/hid_dev.c"
        "comm/ble/hid_device_le_prf.c"
        "comm/ble/hid_bluetooth_manager.c"
    )
endif()

# NRF24 radio stack + HID relay TX — keyboard wireless relay mode only.
# The ESP-NOW config channel that used to ride along was removed with the half
# firmware; this mode now relays HID over NRF24 only.
if(CONFIG_KASE_KBD_WIRELESS)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_pairing.c"
        "comm/rf/kbd_relay_tx.c"
        "comm/usb/usb_presence.c")
endif()

# Dongle CDC stubs (no-op symbols for bt/display referenced by cdc_binary_cmds.c)
# and dongle-specific binary commands (RF status, pair list/reset, battery).
# cr_hmac is kept unconditional: cdc_sec_cmds.c KAT self-test references it directly.
# cr_crc16 is kept unconditional: tiny file, linker-stripped under NONE/OPENPGP.
if(CONFIG_KASE_DEVICE_ROLE_DONGLE)
    list(APPEND srcs
        "comm/cdc/cdc_dongle_stubs.c"
        "comm/cdc/cdc_dongle_cmds.c"
        "security/sec_store.c"
        "comm/cdc/cdc_sec_cmds.c"
        "security/cr_crc16.c"
        "security/cr_hmac.c"
    )
    # OTP transport (YubiKey CR-HMAC personality only)
    if(CONFIG_KASE_SEC_OTP_HID)
        list(APPEND srcs
            "security/otp_proto.c"
            "security/otp_hid.c"
        )
    endif()
    # OpenPGP smartcard personality: CCID class driver + applet + crypto
    if(CONFIG_KASE_SEC_OPENPGP)
        list(APPEND srcs
            "security/ccid.c"
            "security/openpgp_card.c"
            "security/apdu.c"
            "security/openpgp_do.c"
            "security/openpgp_crypto.c"
        )
    endif()
endif()

# Bouchons CDC des rôles sans écran ni BLE ni moteur keymap : l'esclave
# Niphargus et la souris Conchodytes ont exactement les mêmes trous, donc le
# même fichier de bouchons. Le nom du fichier ne mentionne que le premier des
# deux — voir son en-tête.
if(CONFIG_KASE_DEVICE_ROLE_NIPHAR_SLAVE OR CONFIG_KASE_DEVICE_ROLE_MOUSE)
    list(APPEND srcs "comm/cdc/cdc_niphar_slave_stubs.c")
endif()

# RF RX (NRF24) — dongle role only
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_rx_task.c"
        "comm/rf/dongle_state.c"
        # Plus de mapping trackpad ici : la moitié gauche applique la gestuelle
        # elle-même (elle doit le faire pour fonctionner en USB sans dongle) et
        # envoie du HID souris déjà fini.
    )
    # Fusion : le moteur keymap embarqué dans le dongle. Gardé en interne par
    # CONFIG_KASE_DONGLE_FUSION ; hors fusion, TU vide, aucun impact.
    if(CONFIG_KASE_DONGLE_FUSION)
        list(APPEND srcs "comm/rf/dongle_engine.c")
    endif()
endif()

# comm/rf/heartbeat.c n'est plus compilé nulle part : la réconciliation par
# bitmap n'a plus lieu sur le dongle, qui ne décode plus aucune matrice. Le
# module et ses tests host restent au dépôt parce que c'est exactement la logique
# dont la moitié gauche aura besoin pour réconcilier la demi-matrice de la
# droite, en phase 2 du Niphargus. Le supprimer serait à réécrire à l'identique.

# Per-set NRF addressing + pairing (Plan RF-1/RF-2) — dongle RX side
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs "comm/rf/rf_pairing.c")
endif()

# Capteur optique PMW3389 — rôle souris uniquement.
#
# pmw3389_srom.c est le FIRMWARE de la puce, 4094 octets téléversés à chaque
# démarrage. Il n'a de sens que sur une carte qui porte le capteur, d'où le
# symbole dédié plutôt qu'une compilation inconditionnelle.
if(CONFIG_KASE_HAS_MOUSE_SENSOR)
    list(APPEND srcs
        "periph/pmw3389.c"
        "periph/pmw3389_srom.c"
    )
endif()

# Entrées de la souris : trois clics SPDT lus par paires NO+NC, encodeur de
# molette en quadrature, et la tâche qui les assemble avec le capteur.
if(CONFIG_KASE_DEVICE_ROLE_MOUSE)
    list(APPEND srcs
        "input/mouse_buttons.c"
        "input/mouse_wheel.c"
        "app/mouse_task.c"
        "comm/cdc/cdc_mouse_stubs.c"
        "comm/rf/mouse_relay_tx.c"
    )
endif()

# Pile radio en émission — la souris et, historiquement, le clavier sans fil.
# rf_driver.c et rf_pairing.c sont déjà tirés par CONFIG_KASE_KBD_WIRELESS ;
# la garde ci-dessous évite de les lister deux fois.
if(CONFIG_KASE_HAS_RF_TX AND NOT CONFIG_KASE_KBD_WIRELESS)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_pairing.c"
    )
endif()

# v2d_sleep.c fait du light-sleep RF en eteignant un OLED et en prenant le
# verrou LVGL : il depend durement de status_display_sleep/wake, donc d'un ecran.
# ⚠ JAMAIS avec la veille B7 (KASE_VEILLE) : depuis que la gauche du Niphargus
# a un ecran (2026-09-14), la garde « sans-fil + ecran » le compilait AUSSI sur
# elle, et DEUX sommeils cohabitaient. Sur un reveil a capture vide (activite non
# rafraichie), V2D detruisait le pilote, coupait la radio, se rendormait, puis
# recreait tout a son propre reveil — la touche de reveil passait dans ce trou
# (« touche de reveil perdue sur la gauche, depuis toujours », banc 2026-09-16 :
# deux matrix_setup a 30 ms d'ecart). Et sa boucle sondait l'USB toutes les 3 s.
if(CONFIG_KASE_KBD_WIRELESS AND CONFIG_KASE_HAS_DISPLAY AND NOT CONFIG_KASE_VEILLE)
    list(APPEND srcs "comm/rf/v2d_sleep.c")
endif()

# Lien filaire TRRS (B2) — transport UART1 + poignee de main 5 V.
if(CONFIG_KASE_LINK_WIRE)
    list(APPEND srcs "comm/link/link_uart.c")
endif()

# Veille hybride (B7) — light sleep puis deep sleep.
if(CONFIG_KASE_VEILLE)
    list(APPEND srcs "power/veille.c")
endif()

# Jauge batterie (ADC de VBAT_SENSE) — moities Niphargus.
if(CONFIG_KASE_BATT_SENSE)
    list(APPEND srcs "power/batt_sense.c")
endif()

# Frequence dynamique (esp_pm) — ne coute rien sans CONFIG_PM_ENABLE.
list(APPEND srcs "power/pm_dfs.c")

# Lien radio inter-moities (B3). rf_driver.c est deja tire par les gardes RF
# ci-dessus quand l'un des deux bouts est actif.
if(CONFIG_KASE_HALF_LINK_TX OR CONFIG_KASE_HALF_LINK_RX)
    list(APPEND srcs "comm/rf/half_link.c")
    if(NOT CONFIG_KASE_KBD_WIRELESS AND NOT CONFIG_KASE_HAS_RF_RX
       AND NOT CONFIG_KASE_HAS_RF_TX AND NOT CONFIG_KASE_NRF_PROBE)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()

# NRF probe (la radio repond-elle ?) — diagnostic de bring-up, explicite.
# Les gardes ci-dessous evitent de lister rf_driver.c deux fois : il est deja
# tire par KBD_WIRELESS, HAS_RF_RX et HAS_RF_TX.
if(CONFIG_KASE_NRF_PROBE)
    list(APPEND srcs "comm/rf/rf_probe.c")
    if(NOT CONFIG_KASE_KBD_WIRELESS AND NOT CONFIG_KASE_HAS_RF_RX
       AND NOT CONFIG_KASE_HAS_RF_TX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()

# NRF line short-circuit bring-up diagnostic — only when explicitly enabled
if(CONFIG_KASE_NRF_LINE_TEST)
    list(APPEND srcs "comm/rf/rf_line_test.c")
endif()

# PRIV_REQUIRES — gate on Kconfig flags
set(priv_requires
    esp_driver_gpio
    esp_timer
    esp_adc           # jauge batterie (batt_sense) — inconditionnel, ~rien sans KASE_BATT_SENSE
    nvs_flash
    nvs_sec_provider  # NVS encryption HMAC scheme (CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC).
                      # Unconditional like esp_wifi/bt above: a conditional require fails on
                      # the first config pass (CONFIG unknown) and the NVS_SEC_* symbols stay
                      # unknown. Compiles to ~nothing when NVS_ENCRYPTION=n.
    unity
    esp_driver_i2c
    app_update
    bt          # BLE HID (keyboard role). MUST be unconditional: with set(COMPONENTS main)
    esp_hid     # a conditional `if(CONFIG_KASE_HAS_BLE)` requires fails to pull bt on a fresh
                # config (CONFIG unknown on first pass) and nothing else pulls it transitively
                # → BT_* Kconfig symbols unknown + esp_bt_defs.h missing. Same fix as esp_wifi.
                # Harmless on dongle/half (BT disabled in their sdkconfig → compiles to ~nothing).
    spi_flash         # esp_flash_get_size / esp_partition_*
    mbedtls           # HMAC-SHA1 for OTP HID (cr_hmac.c, dongle only)
)

if(CONFIG_KASE_LINK_WIRE)
    list(APPEND priv_requires esp_driver_uart)
endif()

if(CONFIG_KASE_HAS_DISPLAY)
    list(APPEND priv_requires esp_lcd)
endif()

if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_KBD_WIRELESS AND NOT CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_NRF_PROBE AND NOT CONFIG_KASE_HAS_RF_RX AND NOT CONFIG_KASE_KBD_WIRELESS)
    list(APPEND priv_requires esp_driver_spi)
endif()

if((CONFIG_KASE_HALF_LINK_TX OR CONFIG_KASE_HALF_LINK_RX)
   AND NOT CONFIG_KASE_HAS_RF_RX AND NOT CONFIG_KASE_KBD_WIRELESS
   AND NOT CONFIG_KASE_NRF_PROBE)
    list(APPEND priv_requires esp_driver_spi)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "sys"
        "app"
        "security"
        "display"
        "display/oled"
        "display/oled/screens"
        "display/round"
        "display/memlcd"
        "led"
        "display/assets"
        "comm"
        "comm/usb"
        "comm/cdc"
        "comm/ble"
        "comm/rf"
        "comm/link"
        "input"
        "periph"
        "periph/trackpad"
        "power"
        "${BOARD_DIR}"
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_KASE_HAS_DISPLAY)
    littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
endif()
