/* Transport du lien filaire TRRS — brick B2.
 *
 * Tout ce qui est décidé est ailleurs : le format dans link_frame.h, la
 * poignée de main du 5 V dans link_handshake.h — logique pure, testée hôte.
 * Ici, seulement ce qui touche le matériel : l'UART1, la broche du load
 * switch, et la présence USB. Ce fichier traduit des événements en
 * link_hs_step() et des actions en gpio_set_level().
 *
 * ── Le câble est droit ───────────────────────────────────────────────────────
 * TX arrive sur TX. UNE moitié échange donc TXD/RXD via la matrice GPIO
 * (BOARD_LINK_SWAP_TX_RX, à 1 sur la gauche). Piloter les deux TX sans ce
 * swap mettrait deux sorties en opposition sur le même fil.
 *
 * ── Sûreté du 5 V ────────────────────────────────────────────────────────────
 * LINK_5V_EN a un pull-down de 100 k : mort par défaut, et ce fichier le met
 * BAS avant toute autre chose. Il ne passe haut que sur une action de la
 * machine d'états, qui ne l'émet qu'après un échange vérifié — voir
 * l'invariant en tête de link_handshake.h.
 *
 * ── Une moitié en charge reste éveillée ──────────────────────────────────────
 * Le récepteur doit répondre aux sondes pour que le 5 V passe. En light sleep
 * son UART est muette : le pair expire (LINK_HS_PEER_TIMEOUT_MS) et rouvre
 * son switch. link_uart_active() sert donc de verrou à la veille, via le
 * paramètre `bloque` de veille_pas(). Un appareil branché ne dort pas. */
#include "link_uart.h"
#include "link_frame.h"
#include "link_handshake.h"
#include "board.h"
#include "usb_presence.h"     /* vbus_debounce_step — inline, sans le .c */
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "link";

#define LINK_BAUD        115200
#define LINK_TICK_MS     10       /* 1 tick FreeRTOS à 100 Hz */
#define LINK_RX_BUF      64

static link_hs_t        s_hs;
static vbus_debounce_t  s_usb_db;
static bool             s_usb_prev;
static uint8_t          s_seq;
static uint8_t          s_rx[LINK_RX_BUF];
static uint16_t         s_rx_len;
static volatile bool    s_active;

/* Compteurs de banc : on ne voit pas le fil, il faut le compter. */
static uint32_t s_probes_tx, s_acks_tx, s_probes_rx, s_acks_rx, s_skips;

static void set_5v(bool on)
{
    gpio_set_level(BOARD_LINK_5V_EN, on ? 1 : 0);
    if (s_active == on) return;   /* l'entretien re-ferme un switch déjà fermé toutes les 200 ms */
    s_active = on;
    ESP_LOGW(TAG, "5 V %s — GPIO%d relu = %d", on ? "FERME" : "ouvert",
             BOARD_LINK_5V_EN, gpio_get_level(BOARD_LINK_5V_EN));
}

static void send_ctrl(uint8_t type)
{
    uint8_t buf[LINK_FRAME_MIN];
    uint16_t n = link_encode_ctrl(buf, type, s_seq++);
    uart_write_bytes(BOARD_LINK_UART_NUM, buf, n);
    if (type == LINK_TYPE_PROBE) s_probes_tx++; else s_acks_tx++;
}

static void apply(link_hs_action_t a)
{
    switch (a) {
    case LINK_HS_ACT_SEND_PROBE:        send_ctrl(LINK_TYPE_PROBE); break;
    case LINK_HS_ACT_ENABLE_5V:         set_5v(true); break;
    case LINK_HS_ACT_DISABLE_5V:        set_5v(false); break;
    case LINK_HS_ACT_ACK_AND_ENABLE_5V: send_ctrl(LINK_TYPE_ACK); set_5v(true); break;
    case LINK_HS_ACT_NONE:              break;
    }
}

/* Vider l'UART et décoder, avec resynchronisation : le décodeur dit combien
 * consommer, on avance d'autant, et un SKIP consomme toujours au moins un
 * octet — pas de boucle infinie sur du bruit. */
static void drain_uart(uint32_t now)
{
    int n = uart_read_bytes(BOARD_LINK_UART_NUM, s_rx + s_rx_len,
                            LINK_RX_BUF - s_rx_len, 0);
    if (n > 0) s_rx_len += (uint16_t)n;

    uint16_t off = 0;
    for (;;) {
        link_frame_t f; uint16_t used = 0;
        link_decode_status_t st = link_decode(s_rx + off, s_rx_len - off, &f, &used);
        if (st == LINK_DECODE_NEED_MORE) break;
        off += used;
        if (st == LINK_DECODE_SKIP) { s_skips++; continue; }
        switch (f.type) {
        case LINK_TYPE_PROBE: s_probes_rx++; apply(link_hs_step(&s_hs, LINK_HS_EV_PROBED,   now)); break;
        case LINK_TYPE_ACK:   s_acks_rx++;   apply(link_hs_step(&s_hs, LINK_HS_EV_PEER_ACK, now)); break;
        default:                             apply(link_hs_step(&s_hs, LINK_HS_EV_PEER_FRAME, now)); break;
        }
    }
    if (off) { memmove(s_rx, s_rx + off, s_rx_len - off); s_rx_len -= off; }
    if (s_rx_len == LINK_RX_BUF) s_rx_len = 0;   /* tampon plein sans trame : bruit, on repart */
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t dernier_bilan = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        /* Présence USB, en FRONT : la machine d'états veut des événements, pas
         * un état. tud_ready(), pas tud_mounted() — sur l'ESP32-S3 mounted
         * reste vrai après un débranchement à chaud. Même leçon que la veille. */
        bool usb_raw = tud_ready();
#if CONFIG_KASE_LINK_FORCE_SOURCE
        usb_raw = true;   /* banc : source forcee, cf. KASE_LINK_FORCE_SOURCE */
#endif
        bool usb = vbus_debounce_step(&s_usb_db, usb_raw, now, 50);
        if (usb != s_usb_prev) {
            s_usb_prev = usb;
            apply(link_hs_step(&s_hs, usb ? LINK_HS_EV_USB_PRESENT : LINK_HS_EV_USB_GONE, now));
            ESP_LOGI(TAG, "USB %s", usb ? "present" : "absent");
        }

        drain_uart(now);
        apply(link_hs_step(&s_hs, LINK_HS_EV_TICK, now));

        if ((uint32_t)(now - dernier_bilan) >= 5000) {
            dernier_bilan = now;
            ESP_LOGI(TAG, "etat=%d 5V=%d GPIO%d=%d | sondes tx %u rx %u | acks tx %u rx %u | bruit %u",
                     (int)s_hs.state, (int)s_hs.en_5v,
                     BOARD_LINK_5V_EN, gpio_get_level(BOARD_LINK_5V_EN),
                     (unsigned)s_probes_tx, (unsigned)s_probes_rx,
                     (unsigned)s_acks_tx, (unsigned)s_acks_rx, (unsigned)s_skips);
        }
        vTaskDelay(1);   /* LINK_TICK_MS : 1 tick réel, pas pdMS_TO_TICKS(10) qui vaut pareil ici */
    }
}

bool link_uart_active(void) { return s_active; }

void link_uart_start(void)
{
    /* 1. Le switch, BAS, avant tout : c'est la seule broche qui peut faire
     *    du mal, et elle ne doit dépendre d'aucune suite. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LINK_5V_EN,
        .mode = GPIO_MODE_INPUT_OUTPUT,   /* relecture possible : on veut VOIR ce qu'on commande */
    };
    gpio_config(&io);
    gpio_set_level(BOARD_LINK_5V_EN, 0);

    /* 2. L'UART, avec le swap sur la moitié qui l'annonce. */
    uart_config_t uc = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* XTAL, pas APB : avec le DFS (CONFIG_PM_ENABLE) l'APB tombe à 40 MHz au
         * repos et une UART cadencée dessus perd son baud entre deux verrous.
         * Le XTAL ne bouge jamais. */
        .source_clk = UART_SCLK_XTAL,
    };
    ESP_ERROR_CHECK(uart_driver_install(BOARD_LINK_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BOARD_LINK_UART_NUM, &uc));
#if BOARD_LINK_SWAP_TX_RX
    const int link_rx_pin = BOARD_LINK_TX;   /* swap : la vraie RX est sur TX */
    ESP_ERROR_CHECK(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_RX, BOARD_LINK_TX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d (SWAP, cable droit)",
             BOARD_LINK_UART_NUM, BOARD_LINK_RX, BOARD_LINK_TX);
#else
    const int link_rx_pin = BOARD_LINK_RX;
    ESP_ERROR_CHECK(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_TX, BOARD_LINK_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d", BOARD_LINK_UART_NUM, BOARD_LINK_TX, BOARD_LINK_RX);
#endif

    /* Pull-up sur la RX. Quand l'autre moitié dort, sa TX flotte : au repos une
     * UART est à l'état HAUT, une ligne qui flotte descend et se fait lire comme
     * un flot de faux octets (91 682 « bruit » comptés sur une capture du
     * 2026-09-12). Le pull-up interne la tient haute — ligne au repos, pas de
     * décodage parasite. Piste aussi contre le couplage du câble TRRS vers les
     * lignes de matrice, soupçonné dans les réveils fantômes. */
    gpio_set_pull_mode(link_rx_pin, GPIO_PULLUP_ONLY);

    link_hs_init(&s_hs);
    memset(&s_usb_db, 0, sizeof(s_usb_db));
    s_usb_prev = false;
    xTaskCreate(link_task, "link", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "lien filaire pret, 5 V ouvert");
}
