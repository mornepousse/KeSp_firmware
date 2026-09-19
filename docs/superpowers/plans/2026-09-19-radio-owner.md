# radio_owner — la puce nRF24 des moitiés a UN propriétaire — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** séparer, sur les deux moitiés du Niphargus, **la puce** (un `rf_radio_t`, un verrou, un mode PTX/PRX/éteinte, l'excursion, le sommeil, le prêt du bus à l'écran) des **politiques** qui s'en servent (droite : demi-matrice + STATUS + repli de cible ; gauche : brut au dongle + écoute USB de la droite + tirage de keymap par ACK), pour que les invariants qui ont cassé trois fois en silence — *un seul mode à la fois*, *une excursion restaure le mode d'avant*, *la FIFO est vidée AVANT d'émettre, jamais après*, *au réveil la réception est réarmée* — soient **testés host** et non plus recopiés dans chaque module.

**Architecture:** `main/comm/rf/radio_owner.c` possède la puce et parle au matériel à travers une petite table d'opérations (`radio_hw_t`) qui pointe sur `rf_driver_*` en production et sur un **faux enregistreur** dans les tests host — la séquence d'appels matériels est l'oracle. Les modules existants (`half_link.c` = la droite, `kbd_relay_tx.c` = la gauche) ne touchent plus `rf_driver` ni de mutex : ils appellent `radio_*`. Le tirage de keymap par ACK payload sort de `kbd_relay_tx.c` dans `keymap_pull.c`. Le sommeil radio devient un hook de la tâche de veille enregistré une fois, par le propriétaire.

**Tech Stack:** ESP-IDF 5.5.2, nRF24L01+ (ESB, DPL, EN_ACK_PAY), FreeRTOS, tests host CMake (`test/`), `scripts/check.sh`.

**Spec:** revue du 2026-09-18 (proposition 4) et plan `2026-09-18-structure-energie.md` Task 9. État de départ après ce plan (Tasks 1-7 faites, `14af8c3e`) : `kbd_relay_tx.c` 718 l. (13 appels `rf_driver_*`, mutex `s_tx_mutex`, `rf_bus_lock` à lui), `half_link.c` 561 l. (10 appels, mutex `s_tx_radio_mux`, `rf_bus_lock` à lui), `rf_driver.c` 856 l. inchangé. Les trois pannes « mauvais canal en silence » (CLAUDE.md « Une puce, un propriétaire ») et la FIFO vidée au retour d'excursion (« Un acquittement ESB ne prouve pas la réception logicielle ») sont toutes des bugs de propriété.

## Global Constraints

- Ce plan est **RF-critique** : chaque tâche qui touche un binaire se prouve au banc contre des ACK réels (« TX n envois, m acquittes » à droite ; « HID->dongle : n remis, m refuses » à gauche ; ≥ 98 %), les deux moitiés tapant par le dongle, veille B7 + réveil avec touche capturée. Une tâche sans preuve n'est pas finie.
- Workflow : `./scripts/check.sh --fast` vert avant chaque commit ; toute source modifiée sans test → ligne au `COMPORTEMENTS.md`. Pre-push = 7 boards. TDD pour la logique pure, test prouvé **mordant**.
- Flash app-only `esptool write_flash 0x20000`, **MAC-vérifié** (gauche `d0:cf:13:21:92:60`, droite `80:b5:4e:eb:5e:08`, dongle `ac:a7:04:18:82:24` sur `/dev/ttyUSB0`) ; console FTDI `/dev/ttyUSB2` via `capture.py` (jamais `cat`). **Jamais `erase_flash` sur les moitiés** : le `set_id` d'appairage est en NVS.
- Toolchain : `nix develop /home/mae/nixos-config#esp-idf --command …`, `IDF_CCACHE_ENABLE=1 IDF_COMPONENT_CHECK_NEW_VERSION=0`. Build : `idf.py -B build_<board> -DBOARD=<board> -DSDKCONFIG=build_<board>/sdkconfig build`. Boards touchés : `niphar_left`, `niphar_right` (+ `kase_v2_debug` qui compile `kbd_relay_tx.c` hors fusion, et `conchodytes`/`kase_dongle` qui utilisent `rf_driver` mais PAS radio_owner).
- Cadences : `power/cadence.h`, rien sous 30 ms au repos (`_Static_assert`). Ne pas changer une cadence dans ce plan.
- Vider la FIFO **avant** une émission depuis PRX, jamais après. Une excursion se termine par un `CMD_FLUSH_RX` (rf_driver_oob_tx) : tout paquet non lu avant est détruit.
- `rf_driver_power_up` ne touche pas à CE : après un réveil en mode PRX, il faut `rearm_rx` — c'est le propriétaire qui s'en souvient, plus l'appelant.
- Les noms de fichiers `half_link.c` et `kbd_relay_tx.c` sont **conservés** (14 includes, 4 docs, tout l'historique git) ; leur en-tête dit ce qu'ils sont devenus. Seul `keymap_pull.c` est un fichier nouveau extrait.
- Commits en français ; `git push origin main`.

---

## Ordre et dépendances

| Tâche | Sujet | Dépend de | Taille | Banc |
|---|---|---|---|---|
| 1 | `radio_owner` : le cœur, testé host contre un faux matériel | — | ½ j | non |
| 2 | La DROITE passe sur radio_owner | 1 | 2 h | droite |
| 3 | La GAUCHE passe sur radio_owner (PTX/PRX dynamique, excursion) | 1 | ½ j | gauche + USB + dongle |
| 4 | `keymap_pull.c` extrait de `kbd_relay_tx.c` | 3 | 1 h | gauche (diverge.py) |
| 5 | Appairage par le propriétaire | 2, 3 | 1 h | revue (pas de carte à désappairer) |
| 6 | Docs : « Une puce, un propriétaire » réécrit | 2-5 | 30 min | — |

2 et 3 sont indépendantes entre elles ; faire la droite d'abord (plus simple, un seul mode).

**Exécuté le 2026-09-19 : les 6 tâches faites et prouvées au banc** (Task 5 absorbée par 2-3 : les tâches d'appairage touchaient `s_radio`, `radio_pair_round` est arrivé avec elles).

---

### Task 1 : `radio_owner` — le cœur, testé host

**Ce que c'est.** Un module qui possède le seul `rf_radio_t` d'une moitié et impose : un mode à la fois, verrou sur toute transaction, excursion qui vide puis restaure, réveil qui réarme. Il ne connaît ni les trames ni les politiques. Il parle au matériel par `radio_hw_t` (pointeurs de fonctions vers `rf_driver_*`) pour être testable : le test remplace la table par un enregistreur et vérifie la **séquence**.

**Files:**
- Create: `main/comm/rf/radio_owner.h`
- Create: `main/comm/rf/radio_owner.c`
- Create: `test/test_radio_owner.c` (+ `test/CMakeLists.txt`, `test/test_main.c`)
- Modify: `main/CMakeLists.txt` (source ajoutée sous `CONFIG_KASE_HALF_LINK_TX OR CONFIG_KASE_KBD_WIRELESS`, cf. l. 276)

**Interfaces (Produces) :**
```c
/* radio_owner.h */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rf_driver.h"

typedef enum { RADIO_ETEINTE = 0, RADIO_PTX, RADIO_PRX } radio_mode_t;

/* Table d'opérations matérielles. Production : rf_driver_* (radio_hw_defaut).
 * Tests host : un enregistreur. */
typedef struct {
    esp_err_t (*init_tx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*set_ptx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*rearm_rx)(rf_radio_t *, const rf_radio_cfg_t *);
    bool      (*send)(rf_radio_t *, const uint8_t *, uint8_t);
    bool      (*send_ap)(rf_radio_t *, const uint8_t *, uint8_t, uint8_t *, uint8_t *);
    bool      (*oob_tx)(rf_radio_t *, uint8_t, const uint8_t[5], const uint8_t *, uint8_t,
                        uint8_t, const uint8_t[5]);
    bool      (*rx_available)(rf_radio_t *);
    uint16_t  (*read_rx)(rf_radio_t *, uint8_t *, uint16_t);
    void      (*power_down)(rf_radio_t *);
    void      (*power_up)(rf_radio_t *);
} radio_hw_t;

typedef void (*radio_rx_cb_t)(const uint8_t *trame, uint16_t n, void *ctx);

/* Init : la puce en PTX vers `cible`. Enregistre le hook de veille (radio).
 * `hw` = NULL → rf_driver_*. Retourne false si la puce est absente. */
bool radio_owner_init(const rf_radio_cfg_t *cible, const radio_hw_t *hw);
bool radio_presente(void);

/* Verrou de la puce : TOUTE transaction, CSN compris. Aussi le prêt du bus SPI
 * à l'écran (rf_bus_lock/unlock/host sont implémentés ici, une seule fois). */
bool radio_lock(uint32_t timeout_ms);
void radio_unlock(void);

/* Mode. PTX vers `cfg` (canal/adresse), ou PRX à l'écoute de `cfg`. Idempotent
 * si déjà dans ce mode vers cette cible. Prend le verrou. */
bool radio_mode_set(radio_mode_t mode, const rf_radio_cfg_t *cfg);
radio_mode_t radio_mode(void);
const rf_radio_cfg_t *radio_cible(void);   /* cfg courante du mode */

/* Émission — PTX SEULEMENT (refusée en PRX : c'est l'excursion qu'il faut).
 * Prend le verrou (timeout_ms), retourne l'ACK. `_ap` rend la charge de l'ACK. */
bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms);
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms);

/* PRX : lire tout ce qui attend et le livrer à `cb`. Prend le verrou. */
void radio_rx_drain(radio_rx_cb_t cb, void *ctx);

/* PRX : émettre AILLEURS puis revenir écouter la cible courante. VIDE la FIFO
 * dans `cb` (peut être NULL : alors elle est perdue, en le disant) AVANT de
 * partir — l'excursion se termine par un FLUSH_RX. Prend le verrou. */
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx);

/* Veille : power-down, verrou GARDÉ ; réveil : power-up puis RÉARMEMENT du mode
 * courant (rf_driver_power_up ne touche pas à CE : sans ça une PRX repart
 * sourde). Appelés par le hook enregistré à l'init. */
void radio_sleep(void);
void radio_wake(void);

/* Compteurs (banc / écran « dongle vu ») : émissions acquittées / refusées. */
void radio_stats(uint32_t *ok, uint32_t *refus);
```
Pour le host : `TEST_HOST` remplace le mutex par un booléen et `esp_err_t`/`rf_radio_t` par les vrais en-têtes (`rf_driver.h` est déjà inclus par `test_rf_packet.c` ? vérifier ; sinon un `test/stubs/rf_driver.h` minimal avec `rf_radio_t`, `rf_radio_cfg_t`, `esp_err_t`, `ESP_OK`).

- [x] **Step 1 : test rouge — l'enregistreur et les quatre invariants**

`test/test_radio_owner.c` :
```c
/* radio_owner : les invariants qui ont cassé trois fois en silence, vérifiés
 * sur la SÉQUENCE d'appels au matériel (faux enregistreur). */
#include "test_framework.h"
#include "../main/comm/rf/radio_owner.h"
#include <string.h>

static char s_trace[512];
static int  s_pending_rx;          /* trames en attente dans la fausse FIFO */
static bool s_ack = true;
#define T(s) strncat(s_trace, s, sizeof s_trace - strlen(s_trace) - 1)

static esp_err_t f_init_tx(rf_radio_t *r, const rf_radio_cfg_t *c) { (void)c; r->present = true; T("init_tx;"); return ESP_OK; }
static void f_set_ptx(rf_radio_t *r, const rf_radio_cfg_t *c)   { (void)r; char b[24]; snprintf(b, sizeof b, "ptx(%02X);", c->channel); T(b); }
static void f_rearm_rx(rf_radio_t *r, const rf_radio_cfg_t *c)  { (void)r; char b[24]; snprintf(b, sizeof b, "prx(%02X);", c->channel); T(b); }
static bool f_send(rf_radio_t *r, const uint8_t *b, uint8_t n)  { (void)r; (void)b; (void)n; T("send;"); return s_ack; }
static bool f_send_ap(rf_radio_t *r, const uint8_t *b, uint8_t n, uint8_t *a, uint8_t *al) { (void)r; (void)b; (void)n; (void)a; *al = 0; T("send_ap;"); return s_ack; }
static bool f_oob(rf_radio_t *r, uint8_t ch, const uint8_t a[5], const uint8_t *b, uint8_t n, uint8_t rch, const uint8_t ra[5])
{ (void)r; (void)a; (void)b; (void)n; (void)ra; char t[32]; snprintf(t, sizeof t, "oob(%02X->%02X);", ch, rch); T(t); return s_ack; }
static bool f_rx_avail(rf_radio_t *r) { (void)r; return s_pending_rx > 0; }
static uint16_t f_read_rx(rf_radio_t *r, uint8_t *b, uint16_t n) { (void)r; (void)n; if (!s_pending_rx) return 0; s_pending_rx--; b[0] = 0xAB; T("read;"); return 1; }
static void f_pd(rf_radio_t *r) { (void)r; T("pd;"); }
static void f_pu(rf_radio_t *r) { (void)r; T("pu;"); }
static const radio_hw_t FAKE = { f_init_tx, f_set_ptx, f_rearm_rx, f_send, f_send_ap, f_oob, f_rx_avail, f_read_rx, f_pd, f_pu };

static rf_radio_cfg_t cfg(uint8_t ch) { rf_radio_cfg_t c = {0}; c.channel = ch; c.addr_suffix = 0x01; return c; }
static void reset(void) { s_trace[0] = 0; s_pending_rx = 0; s_ack = true; }
static int s_livrees; static void compte(const uint8_t *t, uint16_t n, void *ctx) { (void)t; (void)n; (void)ctx; s_livrees++; }

static void test_init_est_ptx_vers_la_cible(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68);
    TEST_ASSERT(radio_owner_init(&d, &FAKE), "puce presente");
    TEST_ASSERT(radio_mode() == RADIO_PTX, "PTX au depart");
    TEST_ASSERT(radio_cible()->channel == 0x68, "vers la cible");
    TEST_ASSERT(strcmp(s_trace, "init_tx;") == 0, s_trace);
}

static void test_un_seul_mode_a_la_fois_et_idempotent(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    TEST_ASSERT(radio_mode_set(RADIO_PRX, &l), "PRX");
    TEST_ASSERT(radio_mode() == RADIO_PRX && radio_cible()->channel == 0x4F, "ecoute 4F");
    TEST_ASSERT(radio_mode_set(RADIO_PRX, &l), "PRX encore");
    TEST_ASSERT(strcmp(s_trace, "prx(4F);") == 0, "idempotent : une seule ecriture");
    TEST_ASSERT(radio_mode_set(RADIO_PTX, &d), "retour PTX");
    TEST_ASSERT(strcmp(s_trace, "prx(4F);ptx(68);") == 0, s_trace);
}

static void test_emettre_en_prx_est_refuse(void)
{
    /* L'incident : émettre pendant qu'on écoute écrase la config PRX en silence.
     * Le propriétaire refuse, l'appelant doit passer par l'excursion. */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0;
    uint8_t b[4] = {0};
    TEST_ASSERT(!radio_send(b, sizeof b, 20), "send refuse en PRX");
    TEST_ASSERT(strcmp(s_trace, "") == 0, "rien n'a touche la puce");
}

static void test_excursion_vide_la_fifo_AVANT_et_revient(void)
{
    /* L'incident du 2026-09-08 : l'excursion finissait par FLUSH_RX et detruisait
     * des trames deja ACQUITTEES. Ordre exige : read* puis oob(vers->retour). */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0; s_pending_rx = 2; s_livrees = 0;
    uint8_t b[4] = {0}; uint8_t addr[5] = {'K','a','S','e',0x01};
    TEST_ASSERT(radio_excursion_tx(0x68, addr, b, sizeof b, compte, NULL), "excursion ok");
    TEST_ASSERT(s_livrees == 2, "les deux trames livrees");
    TEST_ASSERT(strcmp(s_trace, "read;read;oob(68->4F);") == 0, s_trace);
    TEST_ASSERT(radio_mode() == RADIO_PRX && radio_cible()->channel == 0x4F, "toujours a l'ecoute apres");
}

static void test_excursion_en_ptx_est_refusee(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    uint8_t b[4] = {0}; uint8_t addr[5] = {0};
    TEST_ASSERT(!radio_excursion_tx(0x4F, addr, b, sizeof b, NULL, NULL), "pas d'excursion depuis PTX : send suffit");
    TEST_ASSERT(strcmp(s_trace, "") == 0, "rien");
}

static void test_reveil_rearme_le_mode(void)
{
    /* CLAUDE.md : power_up ne touche pas a CE — une PRX repartait sourde. */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0;
    radio_sleep(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;prx(4F);") == 0, s_trace);
    s_trace[0] = 0; radio_mode_set(RADIO_PTX, &d); s_trace[0] = 0;
    radio_sleep(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;ptx(68);") == 0, "PTX aussi rearme");
}

static void test_le_verrou_est_tenu_pendant_le_sommeil(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE);
    radio_sleep();
    TEST_ASSERT(!radio_lock(0), "personne n'emet sur une puce eteinte");
    radio_wake();
    TEST_ASSERT(radio_lock(0), "libre au reveil"); radio_unlock();
}

static void test_compteurs(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE);
    uint8_t b[4] = {0}; uint32_t ok, refus;
    radio_send(b, 4, 20); s_ack = false; radio_send(b, 4, 20); radio_send(b, 4, 20);
    radio_stats(&ok, &refus);
    TEST_ASSERT(ok == 1 && refus == 2, "1 ok, 2 refus");
}

void test_radio_owner(void)
{
    TEST_SUITE("radio_owner : une puce, un proprietaire");
    TEST_RUN(test_init_est_ptx_vers_la_cible);
    TEST_RUN(test_un_seul_mode_a_la_fois_et_idempotent);
    TEST_RUN(test_emettre_en_prx_est_refuse);
    TEST_RUN(test_excursion_vide_la_fifo_AVANT_et_revient);
    TEST_RUN(test_excursion_en_ptx_est_refusee);
    TEST_RUN(test_reveil_rearme_le_mode);
    TEST_RUN(test_le_verrou_est_tenu_pendant_le_sommeil);
    TEST_RUN(test_compteurs);
}
```
Câbler : `test/CMakeLists.txt` → `test_radio_owner.c` et `../main/comm/rf/radio_owner.c` dans la liste des sources ; `test/test_main.c` → `extern void test_radio_owner(void);` + appel. `TEST_SUITE`/`TEST_RUN`/`TEST_ASSERT` sont les macros de `test_framework.h` (pas `RUN_TEST`).

- [x] **Step 2 : rouge**

`./scripts/check.sh --fast` → rouge (`radio_owner.h` absent).

- [x] **Step 3 : implémentation**

`main/comm/rf/radio_owner.c` :
```c
/* La puce nRF24 d'une moitié du Niphargus — et rien d'autre. Voir radio_owner.h.
 *
 * Trois pannes « mauvais canal en silence » et une FIFO vidée au retour
 * d'excursion (CLAUDE.md « Une puce, un propriétaire », « Un acquittement ESB
 * ne prouve pas la réception logicielle ») : toutes des modules qui écrivaient
 * la config de la puce chacun de leur côté. Ici un seul état (mode + cible),
 * un seul verrou, et les invariants sont des fonctions — testées host contre
 * un faux matériel (test/test_radio_owner.c). */
#include "radio_owner.h"
#include "rf_bus.h"
#include "board.h"
#include <string.h>
#include <stdio.h>
#ifndef TEST_HOST
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"
#endif
static const char *TAG = "radio";
static SemaphoreHandle_t s_mux;
static bool lock_take(uint32_t ms) { return s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void lock_give(void)        { if (s_mux) xSemaphoreGive(s_mux); }
static void lock_create(void)      { if (!s_mux) s_mux = xSemaphoreCreateMutex(); }
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
/* Host : le verrou est un booléen — la sémantique « tenu pendant le sommeil »
 * se teste sans FreeRTOS. */
static bool s_tenu;
static bool lock_take(uint32_t ms) { (void)ms; if (s_tenu) return false; s_tenu = true; return true; }
static void lock_give(void)        { s_tenu = false; }
static void lock_create(void)      { s_tenu = false; }
#define LOGW(...) ((void)0)
#endif

static rf_radio_t     s_radio;
static radio_hw_t     s_hw;
static radio_mode_t   s_mode;
static rf_radio_cfg_t s_cible;
static uint32_t       s_ok, s_refus;

#ifndef TEST_HOST
static const radio_hw_t radio_hw_defaut = {
    rf_driver_init_tx, rf_driver_set_ptx, rf_driver_rearm_rx, rf_driver_send, rf_driver_send_ap,
    rf_driver_oob_tx, rf_driver_rx_available, rf_driver_read_rx, rf_driver_power_down, rf_driver_power_up,
};
#endif

bool radio_owner_init(const rf_radio_cfg_t *cible, const radio_hw_t *hw)
{
#ifndef TEST_HOST
    s_hw = hw ? *hw : radio_hw_defaut;
#else
    s_hw = *hw;
#endif
    memset(&s_radio, 0, sizeof s_radio);
    s_mode = RADIO_ETEINTE; s_ok = s_refus = 0;
    lock_create();
    if (s_hw.init_tx(&s_radio, cible) != ESP_OK || !s_radio.present) return false;
    s_cible = *cible; s_mode = RADIO_PTX;
#if !defined(TEST_HOST) && CONFIG_KASE_VEILLE
    static const veille_hook_t hook = { "radio", radio_sleep, radio_wake };
    veille_hook_enregistrer(&hook);
#endif
    return true;
}
bool radio_presente(void) { return s_radio.present; }
bool radio_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void radio_unlock(void) { lock_give(); }
/* Prêt du bus SPI à l'écran : le même verrou. */
bool rf_bus_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void rf_bus_unlock(void) { lock_give(); }
#ifndef TEST_HOST
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
#endif

static bool meme_cible(const rf_radio_cfg_t *a, const rf_radio_cfg_t *b)
{ return a->channel == b->channel && a->addr_suffix == b->addr_suffix && memcmp(a->rx_addr, b->rx_addr, 4) == 0; }

static void appliquer(radio_mode_t mode, const rf_radio_cfg_t *cfg)   /* verrou tenu */
{
    if (mode == RADIO_PTX) s_hw.set_ptx(&s_radio, cfg);
    else                   s_hw.rearm_rx(&s_radio, cfg);
    s_mode = mode; s_cible = *cfg;
}

bool radio_mode_set(radio_mode_t mode, const rf_radio_cfg_t *cfg)
{
    if (!s_radio.present || mode == RADIO_ETEINTE) return false;
    if (!lock_take(50)) return false;
    if (!(mode == s_mode && meme_cible(cfg, &s_cible))) appliquer(mode, cfg);
    lock_give();
    return true;
}
radio_mode_t radio_mode(void) { return s_mode; }
const rf_radio_cfg_t *radio_cible(void) { return &s_cible; }

bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    if (!s_radio.present || s_mode != RADIO_PTX) return false;   /* en PRX : excursion */
    if (!lock_take(timeout_ms)) return false;
    bool ok = s_hw.send(&s_radio, buf, len);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms)
{
    *ack_len = 0;
    if (!s_radio.present || s_mode != RADIO_PTX) return false;
    if (!lock_take(timeout_ms)) return false;
    bool ok = s_hw.send_ap(&s_radio, buf, len, ack, ack_len);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}

static void vider(radio_rx_cb_t cb, void *ctx)   /* verrou tenu, mode PRX */
{
    uint8_t b[32]; uint32_t perdues = 0;
    while (s_hw.rx_available(&s_radio)) {
        uint16_t n = s_hw.read_rx(&s_radio, b, sizeof b);
        if (!n) break;
        if (cb) cb(b, n, ctx); else perdues++;
    }
    if (perdues) LOGW("%lu trame(s) lue(s) sans consommateur avant l'excursion", (unsigned long)perdues);
}
void radio_rx_drain(radio_rx_cb_t cb, void *ctx)
{
    if (!s_radio.present || s_mode != RADIO_PRX || !lock_take(20)) return;
    vider(cb, ctx);
    lock_give();
}
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx)
{
    if (!s_radio.present || s_mode != RADIO_PRX) return false;   /* depuis PTX : radio_send */
    if (!lock_take(20)) return false;
    vider(cb, ctx);                                               /* AVANT : l'excursion finit par FLUSH_RX */
    uint8_t retour[5] = { s_cible.rx_addr[0], s_cible.rx_addr[1], s_cible.rx_addr[2], s_cible.rx_addr[3], s_cible.addr_suffix };
    bool ok = s_hw.oob_tx(&s_radio, canal, addr, buf, len, s_cible.channel, retour);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}

void radio_sleep(void)
{
    if (!s_radio.present) return;
    (void)lock_take(50);              /* GARDÉ pendant tout le sommeil */
    s_hw.power_down(&s_radio);
}
void radio_wake(void)
{
    if (!s_radio.present) return;
    s_hw.power_up(&s_radio);
    if (s_mode != RADIO_ETEINTE) appliquer(s_mode, &s_cible);   /* power_up ne touche pas à CE */
    lock_give();
}
void radio_stats(uint32_t *ok, uint32_t *refus) { if (ok) *ok = s_ok; if (refus) *refus = s_refus; }
```
`main/CMakeLists.txt` ligne 276 : `if(CONFIG_KASE_HALF_LINK_TX)` → ajouter `list(APPEND srcs "comm/rf/radio_owner.c")` dans ce bloc **et** dans le bloc `CONFIG_KASE_KBD_WIRELESS` (une seule fois si les deux : utiliser une variable `KASE_RADIO_OWNER` ou `list(REMOVE_DUPLICATES srcs)` avant `idf_component_register`). Vérifier que `rf_bus_lock` n'est plus défini nulle part ailleurs quand radio_owner est compilé (Tasks 2-3 retirent les copies ; d'ici là, **ne pas** encore compiler radio_owner.c dans les boards — laisser la ligne CMake commentée avec un `# Task 2/3` et l'activer avec elles). Host : `test/CMakeLists.txt` compile `radio_owner.c` avec `-DTEST_HOST` ; `rf_bus.h` inclut `driver/spi_master.h` → sous `TEST_HOST` le `.c` n'inclut pas `rf_bus.h` (guarder l'include).

- [x] **Step 4 : vert, et mordant**

`./scripts/check.sh --fast` → vert. Puis, transitoirement, inverser l'ordre dans `radio_excursion_tx` (oob puis vider) → `test_excursion_vide_la_fifo_AVANT_et_revient` rouge ; rétablir. Puis retirer `appliquer(...)` de `radio_wake` → `test_reveil_rearme_le_mode` rouge ; rétablir.

- [x] **Step 5 : contrat, commit**

`COMPORTEMENTS.md`, nouvelle section `## Niphargus — radio : une puce, un propriétaire` :
```
- [test:test_radio_owner] La puce nRF24 d'une moitié a UN propriétaire
  (`comm/rf/radio_owner.c`) : un mode à la fois (PTX vers une cible, PRX à
  l'écoute d'une cible, éteinte), idempotent ; émettre en PRX est REFUSÉ (c'est
  l'excursion) ; une excursion VIDE la FIFO de réception dans le consommateur
  AVANT de partir et revient écouter la cible d'avant ; le réveil RÉARME le
  mode courant (power_up ne touche pas à CE) ; le verrou est tenu pendant tout
  le sommeil. Vérifié sur la séquence d'appels au matériel (faux enregistreur).
```
```bash
git add main/comm/rf/radio_owner.c main/comm/rf/radio_owner.h test/test_radio_owner.c test/CMakeLists.txt test/test_main.c main/CMakeLists.txt COMPORTEMENTS.md .tripwire-testcount
git commit -m "feat(rf): radio_owner — la puce nRF24 d'une moitié a un propriétaire ; invariants (un mode, excursion vide-puis-revient, réveil réarme, verrou tenu au sommeil) testés host contre un faux matériel"
```

---

### Task 2 : La DROITE passe sur radio_owner

**Constat.** `half_link.c` (561 l.) fait aujourd'hui : `rf_driver_init_tx` (l. 182), `s_tx_radio_mux` créé l. 148 et pris dans `half_link_tx_frame` (l. 327) autour de `rf_driver_send` (l. 333) puis `rf_driver_set_ptx` sur bascule de cible (l. 358/371), `rf_bus_lock/unlock/host` (l. 253-259), `half_link_radio_sleep/wake` (l. 527-542) + hook de veille (l. 191), appairage (l. 121-125, Task 5). Un seul mode : PTX, deux cibles (dongle KaSe.01 / gauche KaSe.03).

**Files:**
- Modify: `main/comm/rf/half_link.c` (init, tx_frame, sleep/wake, rf_bus_lock, hook)
- Modify: `main/comm/rf/half_link.h` (retirer `half_link_radio_sleep/wake` s'ils n'ont plus d'appelant — `grep`)
- Modify: `main/CMakeLists.txt` (activer `radio_owner.c` sous `CONFIG_KASE_HALF_LINK_TX`)
- Modify: `COMPORTEMENTS.md`

**Interfaces (Consumes) :** `radio_owner_init`, `radio_send`, `radio_mode_set(RADIO_PTX, tgt)`, `radio_stats`.

- [x] **Step 1 : init**

Dans `half_link_tx_init`, remplacer la création du mutex (l. 148-152) et `rf_driver_init_tx(&s_radio, &cfg)` (l. 182) par :
```c
    if (!radio_owner_init(&cfg, NULL)) {
        ESP_LOGE(TAG, "TX init echouee — la moitie droite restera muette");
        return false;
    }
```
Supprimer `static rf_radio_t s_radio;` (l. 33) et `static SemaphoreHandle_t s_tx_radio_mux;` (l. 55). Supprimer l'enregistrement du hook (l. 187-193) : c'est le propriétaire qui l'enregistre. Supprimer le bloc `rf_bus_lock/unlock/host` (l. 253-259) et l'include `rf_bus.h`.

- [x] **Step 2 : tx_frame**

`half_link_tx_frame` devient :
```c
static bool half_link_tx_frame(const uint8_t *buf, uint8_t n)
{
    if (!radio_presente()) return false;
    s_seq++;
    /* 50 ms : une émission ESB au pire cas (ARC=15, ARD=500 µs) tient en ~13 ms. */
    bool ack = radio_send(buf, n, 50);
    if (ack) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;
#if CONFIG_KASE_DONGLE_FUSION
    /* Repli / chien de garde : la FSM (pure, testée) décide, le propriétaire
     * applique — réécrire la config PTX réarme aussi une puce figée. La FSM
     * est protégée par s_etat_mux : deux tâches appellent tx_frame. */
    bool bascule;
    taskENTER_CRITICAL(&s_etat_mux);
    bascule = half_tx_target_step(&s_tx_fsm, ack, HALF_TX_SWITCH_FAILS);
    taskEXIT_CRITICAL(&s_etat_mux);
    if (bascule) {
        const rf_radio_cfg_t *tgt = (s_tx_fsm.cible == HALF_TX_TO_LEFT) ? &s_cfg_left : &s_cfg_dongle;
        radio_mode_set(RADIO_PTX, tgt);
        ESP_LOGW(TAG, "repli : bascule TX -> %s (rearme, %u sans ACK)",
                 s_tx_fsm.cible == HALF_TX_TO_LEFT ? "GAUCHE KaSe.03 (heartbeat)" : "DONGLE KaSe.01 (matrix)",
                 (unsigned)HALF_TX_SWITCH_FAILS);
    }
#else
    static uint16_t s_sans_ack;
    if (ack) s_sans_ack = 0;
    else if (++s_sans_ack >= 30) { s_sans_ack = 0; radio_mode_set(RADIO_PTX, &s_tx_cfg); ESP_LOGW(TAG, "chien de garde : radio TX rearmee (30 envois sans ACK)"); }
#endif
    /* bilan tous les 10 envois : inchangé */
    …
    return ack;
}
```
⚠ `radio_mode_set` est **idempotent** : un réarmement vers la MÊME cible (chien de garde hors fusion, ou FSM qui « bascule » vers la cible courante) ne réécrirait rien. Pour forcer la réécriture, ajouter à `radio_owner.h` : `bool radio_rearmer(void);` = ré-applique le mode courant sous verrou (+ un test host `test_rearmer_reecrit_le_mode_courant` : trace `ptx(68);` après `radio_rearmer()` en PTX). L'utiliser dans les deux branches ci-dessus quand la cible ne change pas.
`s_tx_cfg` (l. 57) : ne sert plus qu'au chien de garde hors fusion → remplacer par `radio_rearmer()` et supprimer la variable.

- [x] **Step 3 : sommeil**

Supprimer `half_link_radio_sleep/wake` (l. 527-542) et leurs prototypes ; `s_dernier_status_ms = 0` au réveil (STATUS forcé après un sommeil) migre dans un hook local léger si on y tient :
```c
static void half_link_apres_reveil(void) { s_dernier_status_ms = 0; }
… dans half_link_tx_init : static const veille_hook_t h = { "status", NULL, half_link_apres_reveil }; veille_hook_enregistrer(&h);
```
(le hook radio du propriétaire est enregistré AVANT dans `radio_owner_init` → au réveil, ordre inverse : status puis radio — sans importance, le STATUS partira au tick suivant, radio debout.)

- [x] **Step 4 : compiler, banc droite, contrat, commit**

`main/CMakeLists.txt` : `radio_owner.c` sous `CONFIG_KASE_HALF_LINK_TX`. Build `niphar_right` ; `grep -n "rf_driver_\|xSemaphore" main/comm/rf/half_link.c` → il ne reste que l'appairage (Task 5). Flasher la droite (FTDI dessus, MAC-check), capture : « TX pret », « épreuve : 10/10 », frappe 1 min « TX n envois, n acquittes (≥ 98 %) », veille à 15 s, réveil avec touche capturée et « TX … acquittes » qui reprend juste après (= radio réarmée au réveil), écran intact. Repli : débrancher le dongle 10 s en tapant → « repli : bascule TX -> GAUCHE » puis retour quand le dongle revient (la gauche en USB pour que quelqu'un écoute KaSe.03 — sinon la bascule se fait quand même mais personne n'acquitte : c'est le comportement actuel).
Contrat :
```
- [smoke:Éveil oisif] La DROITE ne touche plus la puce : `half_link.c` ne fait
  que des trames (demi-matrice, STATUS, repli de cible par la FSM pure) et
  passe par radio_owner pour émettre, basculer de cible, réarmer, dormir.
  Banc : ACK ≥ 98 %, repli dongle→gauche→dongle, réveil avec radio debout.
```
```bash
git commit -am "refactor(rf): la droite passe sur radio_owner — plus de rf_driver ni de mutex dans half_link.c"
```

---

### Task 3 : La GAUCHE passe sur radio_owner

**Constat.** `kbd_relay_tx.c` (718 l.) : `rf_driver_init_tx` (l. 551), `s_tx_mutex` partout, `rf_bus_lock` (l. 145-150), `kbd_tx_locked` = `send_ap` + décodage sync (l. 152-215), le corps de rafraîchissement (l. 250-335) : bascule `rearm_rx` (PRX KaSe.03 en USB) / `set_ptx` (retour), vidange de FIFO manuelle (l. 274-283), annonce par `oob_tx` (l. 308), sommeil (l. 625-640). C'est le module des trois pannes.

**Files:**
- Modify: `main/comm/rf/kbd_relay_tx.c`
- Modify: `main/comm/rf/kbd_relay_tx.h` (retirer `kbd_relay_sleep_prepare/wake_restore`)
- Modify: `main/CMakeLists.txt` (activer `radio_owner.c` sous `CONFIG_KASE_KBD_WIRELESS`, sans doublon avec Task 2)
- Modify: `COMPORTEMENTS.md`

**Interfaces (Consumes) :** `radio_owner_init`, `radio_send_ap`, `radio_mode_set(RADIO_PRX|RADIO_PTX, …)`, `radio_rx_drain`, `radio_excursion_tx`, `radio_stats`.

- [x] **Step 1 : init et émission**

`kbd_relay_init` : remplacer `rf_driver_init_tx(&s_radio, &nrf_cfg)` et la création de `s_tx_mutex` (l. 571) par `if (!radio_owner_init(&nrf_cfg, NULL)) { ESP_LOGE(…); return; }`. Supprimer `s_radio`, `s_tx_mutex`, le bloc `rf_bus_lock` (l. 145-150), l'enregistrement du hook (l. 583-589), `kbd_relay_sleep_prepare/wake_restore` (l. 625-640) — le timer de rafraîchissement n'a plus besoin d'être arrêté au sommeil : ses appels au propriétaire échouent proprement (verrou tenu, `radio_send` → false, compté en refus… ⚠ NON : un refus compté pendant le sommeil fausserait « dongle vu »). Donc garder un hook local : `{ "relais", kbd_relay_dormir, kbd_relay_reveiller }` qui stoppe/relance le timer (`esp_timer_stop`, `s_periode_ms = 0; kbd_relay_timer_set(KBD_RELAY_REFRESH_MS)`), sans toucher la radio.
`kbd_tx_locked` → `kbd_tx` : plus de mutex ; `bool ok = radio_send_ap(buf, len, ack, &ack_n, 20);` ; si `radio_send_ap` retourne false **parce que le mode est PRX** (route USB) c'est attendu (le callback de scan n'émet qu'en route RF — vérifier que `s_tx_sans_mutex` n'est plus alimenté : le remplacer par un compteur « refus de mode » ou le supprimer).

- [x] **Step 2 : la bascule de mode (le cœur)**

Corps de `kbd_relay_refresh_body`, branche USB :
```c
    if (kbd_active_route() == KBD_OUT_USB) {
        rf_radio_cfg_t link = s_kbd_cfg; link.channel = RF_CH_HALF_LINK; link.addr_suffix = RF_ADDR_HALF_LINK;
        if (radio_mode() != RADIO_PRX) {
            if (!radio_mode_set(RADIO_PRX, &link)) return;
            ESP_LOGW(TAG, "fusion USB : ecoute la droite reemise (PRX ch=0x%02X KaSe.%02X)", RF_CH_HALF_LINK, RF_ADDR_HALF_LINK);
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        radio_rx_drain(kbd_relay_rx_droite, &now);        /* remplace la boucle rx_available/read_rx */
        … silence de la droite : inchangé …
        if ((uint32_t)(now - s_derniere_emission_ms) >= 200u) {
            … construire sb/sn/dst : inchangé …
            bool ok = radio_excursion_tx(s_kbd_cfg.channel, dst, sb, (uint8_t)sn, kbd_relay_rx_droite, &now);
            if (ok) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;
            s_derniere_emission_ms = now;
        }
        return;
    }
    if (radio_mode() == RADIO_PRX) {                        /* retour sans-fil */
        radio_mode_set(RADIO_PTX, &s_kbd_cfg);
        memset(s_remote_bm, 0, RF_HALF_BITMAP_BYTES); s_remote_changed = true;
        ESP_LOGW(TAG, "fusion : retour emission PTX vers le dongle");
    }
```
avec le consommateur :
```c
static void kbd_relay_rx_droite(const uint8_t *rb, uint16_t rn, void *ctx)
{
    uint32_t now = *(uint32_t *)ctx; rf_heartbeat_t h;
    if (!rf_decode_heartbeat(rb, rn, &h)) return;
    if (memcmp(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES) != 0) { memcpy(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES); s_remote_changed = true; }
    s_remote_ms = now;
}
```
`s_usb_listening` disparaît : c'est `radio_mode() == RADIO_PRX`. `kbd_relay_cadence_ms(…, ecoute_usb)` reçoit `radio_mode() == RADIO_PRX`.
⚠ L'annonce par excursion passe désormais le **consommateur** : c'est exactement l'invariant testé en Task 1 — les trames de la droite arrivées juste avant l'annonce ne sont plus perdues (aujourd'hui `rf_driver_oob_tx` l. 308 est appelé APRÈS la vidange manuelle l. 274, c'est correct, mais rien ne l'impose ; désormais le propriétaire l'impose).

- [x] **Step 3 : compiler, banc gauche, contrat, commit**

`grep -n "rf_driver_\|xSemaphore\|s_tx_mutex" main/comm/rf/kbd_relay_tx.c` → il ne reste que l'appairage (Task 5). Build `niphar_left` **et** `kase_v2_debug` (compile `kbd_relay_tx.c` hors fusion : la branche `#else` de `kbd_relay_cadence_ms` et le V2D). Banc gauche, FTDI dessus, dans l'ordre :
1. batterie : frappe 1 min → « HID->dongle : n remis, m refuses » ≥ 98 %, veille à 15 s, réveil avec touche capturée et relais reparti ;
2. USB branché (câble data !) : « fusion USB : ecoute la droite reemise », `lsusb` cafe:4003, la gauche tape en local, **la droite passe par la gauche** (le dongle réémet) — taper sur les deux, aucune touche brève perdue côté droite (c'est le tick à 10 ms + la vidange dans le consommateur) ;
3. débrancher : « retour emission PTX vers le dongle », frappe des deux moitiés par le dongle, pas de touche collée ;
4. sync : `diverge.py` (scratchpad) → « sync keymap : balise … pull » → « 40/40 recus … enregistree en NVS » → `coh.py` match=1 ; `restore` → retour.
Contrat :
```
- [smoke:Fusion — moteur local dormant] La GAUCHE ne touche plus la puce :
  `kbd_relay_tx.c` demande au propriétaire PTX vers le dongle (sans-fil) ou PRX
  sur KaSe.03 (USB), livre ses trames (brut, STATUS, REQ de sync) par
  radio_send_ap et son annonce USB par radio_excursion_tx — qui vide la FIFO
  des trames de la droite dans le consommateur AVANT de partir. Banc : les
  quatre scénarios (batterie, USB simultané, retour, sync) sans perte.
```
```bash
git commit -am "refactor(rf): la gauche passe sur radio_owner — PTX/PRX par le propriétaire, excursion qui vide dans le consommateur, plus de mutex ni de rf_driver dans kbd_relay_tx.c"
```

---

### Task 4 : `keymap_pull.c` extrait

**Constat.** Le tirage de keymap par ACK payload occupe ~36 lignes de `kbd_relay_tx.c` réparties en trois endroits (décodage balise/chunk dans `kbd_tx`, REQ toutes les 100 ms dans le rafraîchissement, enregistrement NVS + veto). Il a sa logique pure (`keymap_sync.h`, testée) ; ce qui manque est un module qui la porte.

**Files:**
- Create: `main/comm/rf/keymap_pull.c`, `main/comm/rf/keymap_pull.h`
- Modify: `main/comm/rf/kbd_relay_tx.c` (retirer `s_krx`, `s_syncing`, `s_sync_target_fp`, `s_sync_done`, les trois blocs), `main/CMakeLists.txt` (sous `CONFIG_KASE_DONGLE_FUSION AND CONFIG_KASE_KBD_WIRELESS`)

**Interfaces (Produces) :**
```c
/* keymap_pull.h — tirage de la keymap du dongle par ACK payload (gauche, fusion). */
void keymap_pull_init(void);
/* À appeler avec la charge de CHAQUE ACK reçu (balise ou chunk). */
void keymap_pull_on_ack(const uint8_t *ack, uint8_t n);
/* Un tirage est-il en cours ? (cadence rapide du relais, priorité aux maintiens) */
bool keymap_pull_en_cours(void);
/* Tick du relais : émet un REQ au plus toutes les 100 ms tant qu'on tire ;
 * enregistre en NVS quand 40/40 sont là. `emettre` = radio_send_ap du relais. */
void keymap_pull_tick(bool (*emettre)(const uint8_t *, uint8_t));
```
Le veto `VEILLE_VETO_SYNC` est posé/levé dans ce module. `kbd_relay_tx.c` appelle `keymap_pull_on_ack(ack, ack_n)` après `radio_send_ap` et `keymap_pull_tick(kbd_relay_emettre_req)` dans le rafraîchissement ; `kbd_relay_cadence_ms(…, sync = keymap_pull_en_cours(), …)`.

- [x] Step 1 : créer le module par déplacement littéral des trois blocs (pas de réécriture), compiler `niphar_left`.
- [x] Step 2 : banc gauche — `diverge.py` → pull → « enregistree en NVS » → `coh.py` match=1 ; `restore` → retour ; en pleine frappe, aucune sensation de perte (cf. validation du 2026-09-13).
- [x] Step 3 : contrat (ligne `[smoke:…sync…]` existante : ajouter « porté par `keymap_pull.c` ») ; commit `refactor(rf): keymap_pull.c — le tirage de keymap par ACK sort du relais`.

---

### Task 5 : Appairage par le propriétaire

**Constat.** Les deux tâches d'appairage (`kbd_pairing_task` l. ~486-545 de `kbd_relay_tx.c`, `half_fusion_pairing_task` l. ~104-140 de `half_link.c`) font `rf_driver_set_tx_address` + `rf_driver_set_channel` + `rf_driver_send` + `rf_driver_pair_listen` sur la puce directement — le dernier endroit hors propriétaire, et un cas où « émettre sur le rendez-vous » est précisément un changement de cible.

**Files:**
- Modify: `main/comm/rf/radio_owner.h/.c` : `bool radio_pair_round(const uint8_t rdv_addr[5], uint8_t rdv_ch, const uint8_t *req, uint8_t n, uint8_t *rx, uint16_t rx_max, uint32_t listen_ms, uint16_t *rx_n);` — sous verrou : cible temporaire (adresse + canal de rendez-vous), `send`, `pair_listen`, puis **retour à la cible courante** (`appliquer(s_mode, &s_cible)`). Table `radio_hw_t` : ajouter `set_tx_address`, `set_channel`, `pair_listen`.
- Test host : `test_pair_round_revient_a_la_cible` (trace : `…pair_listen;ptx(68);`).
- Modify : les deux tâches d'appairage appellent `radio_pair_round`.

- [x] Step 1 : test rouge, implémentation, vert, mordant (retirer le retour → rouge).
- [x] Step 2 : compiler les deux moitiés ; `grep -rn "rf_driver_" main/comm/rf/kbd_relay_tx.c main/comm/rf/half_link.c` → **vide**.
- [x] Step 3 : banc : **pas de désappairage des cartes en service** (set_id en NVS, jamais d'erase). Vérification par revue + test host ; si une carte de rechange existe : `KS_CMD_RF_PAIR_START` sur le dongle + carte vierge → « appairage : ACK set_id=… — sauvegarde + reboot ».
- [x] Step 4 : contrat `[test:test_radio_owner]` (ajouter « un tour d'appairage revient à la cible courante ») ; commit `refactor(rf): l'appairage passe par radio_owner — plus aucun rf_driver_* hors du propriétaire sur les moitiés`.

---

### Task 6 : Docs

- [x] `CLAUDE.md` « ⚠ Une puce, un propriétaire » : réécrire — la radio de chaque moitié appartient à `comm/rf/radio_owner.c` ; `half_link.c` (droite) et `kbd_relay_tx.c` (gauche) sont des politiques qui ne voient ni `rf_driver` ni mutex ; les invariants sont dans `test_radio_owner` ; toute nouvelle écriture de config de la puce passe par `radio_mode_set`/`radio_rearmer`. Retirer la mention de `half_link_excursion_tx` (n'existe plus). Arbre : ajouter `radio_owner.c`, `keymap_pull.c`.
- [x] `README.md` : une phrase dans le paragraphe « Power policy given a home » ou un nouveau court paragraphe « The radio has one owner ».
- [x] Mémoire : `fusion-dongle-et-usb-gauche.md` — remplacer « La radio de la gauche bascule dynamiquement PTX↔PRX (`rf_driver_set_ptx`/`rf_driver_rearm_rx` dans `kbd_relay_refresh_cb`) » par la référence à `radio_mode_set`.
- [x] Commit `docs: la radio des moitiés a un propriétaire (radio_owner)`, push.

---

## Self-review

- **Couverture** : proposition 4 de la revue (propriétaire + politiques minces + invariants testés) → Tasks 1-5 ; `rf_bus_lock` dupliqué → Task 1 (une seule implémentation) ; `keymap_pull` → Task 4 ; appairage → Task 5.
- **Placeholders** : aucun « TBD » ; le code de Task 1 est complet ; Tasks 2-3 donnent les corps des fonctions changées ; Task 4 est un déplacement littéral (décidé comme tel).
- **Cohérence des noms** : `radio_owner_init`, `radio_mode_set`, `radio_rearmer` (introduit en Task 2, à ajouter au header de Task 1 dès qu'on y arrive), `radio_send/_ap`, `radio_rx_drain`, `radio_excursion_tx`, `radio_sleep/wake`, `radio_stats`, `radio_pair_round` (Task 5) ; `keymap_pull_*` (Task 4).
- **Risques nommés** : (a) `kase_v2_debug` compile `kbd_relay_tx.c` hors fusion — le build des 7 boards au pre-push l'attrape ; (b) le timer de rafraîchissement continue de tourner pendant le sommeil si le hook local du relais est oublié → refus comptés, « dongle vu » faussé : Task 3 Step 1 le prévoit ; (c) la stat `radio_stats` du propriétaire et les compteurs du relais (`s_tx_remis/refuses`) font doublon — garder ceux du relais pour le journal existant, `radio_stats` pour l'écran, ou fusionner en Task 3 ; (d) pas de banc pour l'appairage sans carte à sacrifier — assumé.
