/* Backend display_backend_t du Sharp memory-LCD — UI LVGL en portrait 68 × 160 :
 *   bandeau  : route (RF/USB) + ▲ si le dongle acquitte, jauge + tension locale
 *   centre   : GAUCHE = nom de couche en lignes de 4 (memlcd_couper_nom) + « Ln »
 *              DROITE = logo Niphargus 60 px centré
 *   pied     : l'AUTRE moitié : « DROITE »/« GAUCHE », sa tension, ■ pleine, ? inconnue
 * LVGL rend en 16 bits dans un tampon plein écran (full_refresh) ; le flush
 * seuille vers le tampon 1 bit portrait et memlcd_panel_show() l'écrit sous le
 * verrou du bus radio. Si la radio tient le bus, le flush garde l'image et
 * update() la repousse au tick suivant : aucune image n'est perdue, elle est
 * juste retardée de 100 ms.
 *
 * ⚠ Ordre de boot : sur la gauche « display init » précède l'init de la radio,
 * et c'est la RADIO qui crée le bus SPI (rf_driver, shares_bus_first). Ajouter
 * le device écran avant elle rend ESP_ERR_INVALID_STATE (vu au banc : écran à
 * 602 ms, radio à 652 ms). L'attachement est donc DIFFÉRÉ au premier update()
 * qui trouve le bus ; LVGL, lui, est construit tout de suite. */
#include "memlcd_backend.h"
#include "memlcd_panel.h"
#include "status_display.h"
#include "board.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#include "batt_calc.h"
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "keyboard_config.h"   /* current_layout, default_layout_names */
#endif
#if CONFIG_KASE_KBD_WIRELESS
#include "usb_presence.h"      /* kbd_active_route */
#include "kbd_relay_tx.h"      /* kbd_relay_dongle_vu */
#endif
#if CONFIG_KASE_HALF_LINK_TX
#include "half_link.h"         /* half_link_tx_dongle_vu */
#endif

static const char *TAG = "memlcd_be";
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_unscii_8);
extern const lv_img_dsc_t img_niphargus_60;

/* ── État ─────────────────────────────────────────────────────────── */
static bool s_attached;                 /* panneau sur le bus */
static bool s_built;                    /* objets LVGL construits */
static volatile bool s_sleeping;        /* image gelée : ni flush ni VCOM */
static volatile bool s_dirty;           /* image seuillée pas encore poussée */
static uint8_t s_fb[MEMLCD_H * MEMLCD_LINE_BYTES];            /* portrait, 1 = encre */
static lv_color_t s_draw[MEMLCD_W * MEMLCD_H];                 /* rendu LVGL plein écran */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_drv;
static lv_disp_t *s_disp;
static memlcd_model_t s_shown;          /* dernier modèle dessiné */

/* Données distantes (trame DISPLAY) — scalaires, écrits par la tâche radio. */
static volatile uint8_t s_r_couche, s_r_batt_dv = 0xFF, s_r_batt_chg, s_r_dongle_ok;
static volatile bool    s_r_valide;

void memlcd_backend_set_remote(uint8_t couche, uint8_t batt_autre_dv, uint8_t batt_autre_chg, bool dongle_ok)
{
    s_r_couche = couche; s_r_batt_dv = batt_autre_dv; s_r_batt_chg = batt_autre_chg;
    s_r_dongle_ok = dongle_ok; s_r_valide = true;
}

/* ── Objets LVGL ──────────────────────────────────────────────────── */
static lv_obj_t *s_l_route, *s_bar, *s_l_volt, *s_l_nom[MEMLCD_NOM_LIGNES], *s_l_couche,
                *s_l_autre, *s_l_autre_v;

#define Y_BANDEAU_FIN 31
#define Y_PIED_DEBUT  132

static void trait(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, y); lv_obj_set_size(o, MEMLCD_W, 1);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t *texte(lv_obj_t *parent, const lv_font_t *f, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void construire(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Bandeau : route + ▲ (Montserrat 14 porte les symboles), jauge à droite,
     * tension dessous en UNSCII 8. */
    s_l_route = texte(scr, &lv_font_montserrat_14, 3, 2);
    s_l_volt  = texte(scr, &lv_font_unscii_8, 3, 20);
    s_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, 10, 24); lv_obj_set_pos(s_bar, MEMLCD_W - 14, 3);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_border_color(s_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    trait(scr, Y_BANDEAU_FIN);

    /* Centre */
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) {
        s_l_nom[i] = texte(scr, &lv_font_montserrat_14, 0, 48 + i * 18);
        lv_obj_set_width(s_l_nom[i], MEMLCD_W);
        lv_obj_set_style_text_align(s_l_nom[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    s_l_couche = texte(scr, &lv_font_unscii_8, 0, 110);
    lv_obj_set_width(s_l_couche, MEMLCD_W);
    lv_obj_set_style_text_align(s_l_couche, LV_TEXT_ALIGN_CENTER, 0);
#else
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, &img_niphargus_60);
    lv_obj_set_style_img_recolor(img, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    /* centré dans la zone entre les deux traits */
    lv_obj_set_pos(img, (MEMLCD_W - 60) / 2,
                   Y_BANDEAU_FIN + 1 + ((Y_PIED_DEBUT - Y_BANDEAU_FIN - 1) - 60) / 2);
#endif

    /* Pied : l'autre moitié */
    trait(scr, Y_PIED_DEBUT);
    s_l_autre   = texte(scr, &lv_font_unscii_8, 3, Y_PIED_DEBUT + 5);
    s_l_autre_v = texte(scr, &lv_font_unscii_8, 3, Y_PIED_DEBUT + 17);
    s_built = true;
}

/* ── Modèle : ce que les sources disent maintenant ────────────────── */
static void lire_modele(memlcd_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->batt_local_dv = 0xFF; m->batt_autre_dv = s_r_batt_dv; m->batt_autre_chg = s_r_batt_chg;
#if CONFIG_KASE_BATT_SENSE
    { uint8_t dv = batt_sense_dv(); m->batt_local_dv = dv ? dv : 0xFF; m->batt_local_chg = batt_sense_charging(); }
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    m->is_left   = 1;
#if CONFIG_KASE_KBD_WIRELESS
    m->route_rf  = (kbd_active_route() == KBD_OUT_RF);
    m->dongle_vu = kbd_relay_dongle_vu();
#endif
    m->couche    = current_layout;
    strncpy(m->nom, default_layout_names[current_layout], sizeof m->nom - 1);
#else
    m->is_left   = 0;
    m->route_rf  = 1;
#if CONFIG_KASE_HALF_LINK_TX
    m->dongle_vu = half_link_tx_dongle_vu() || s_r_dongle_ok;
#else
    m->dongle_vu = s_r_dongle_ok;
#endif
    m->couche    = s_r_couche;
#endif
}

static void tension(char *out, size_t n, uint8_t dv, uint8_t chg)
{
    if (dv == 0xFF) snprintf(out, n, "?");
    else snprintf(out, n, "%u.%uV%s", dv / 10, dv % 10, chg == 2 ? " #" : (chg == 1 ? " +" : ""));
}

static void dessiner(const memlcd_model_t *m)
{
    char buf[24];
    lv_label_set_text_fmt(s_l_route, "%s%s", m->route_rf ? "RF" : "USB", m->dongle_vu ? " " LV_SYMBOL_UP : "");
    tension(buf, sizeof buf, m->batt_local_dv, m->batt_local_chg);
    lv_label_set_text(s_l_volt, buf);
    uint8_t pct = 0;
#if CONFIG_KASE_BATT_SENSE
    pct = (m->batt_local_dv == 0xFF) ? 0 : batt_soc_pct(m->batt_local_dv);
#endif
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    memlcd_couper_nom(m->nom, lignes);
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lv_label_set_text(s_l_nom[i], lignes[i]);
    lv_label_set_text_fmt(s_l_couche, "L%u", (unsigned)m->couche);
#endif
    lv_label_set_text(s_l_autre, m->is_left ? "DROITE" : "GAUCHE");
    tension(buf, sizeof buf, m->batt_autre_dv, m->batt_autre_chg);
    lv_label_set_text(s_l_autre_v, buf);
}

/* ── LVGL → panneau ───────────────────────────────────────────────── */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    /* full_refresh : l'aire est tout l'écran, px est ligne par ligne 68 large */
    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        uint8_t *row = &s_fb[y * MEMLCD_LINE_BYTES];
        for (lv_coord_t x = area->x1; x <= area->x2; x++, px++) {
            if (lv_color_brightness(*px) < 128) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            else                                row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        if (s_sleeping || !s_attached || !memlcd_panel_show(s_fb)) s_dirty = true;   /* repoussé par update() */
        else s_dirty = false;
    }
    lv_disp_flush_ready(drv);
}

static bool lvgl_pret(void)
{
    if (s_disp) return true;
    if (!lv_is_initialized()) {
        const lvgl_port_cfg_t cfg = { .task_priority = 2, .task_stack = 6144, .task_affinity = 0,
                                      .task_max_sleep_ms = 100, .timer_period_ms = 20 };
        if (lvgl_port_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "lvgl_port_init KO"); return false; }
    }
    if (!lvgl_port_lock(200)) return false;
    lv_disp_draw_buf_init(&s_draw_buf, s_draw, NULL, MEMLCD_W * MEMLCD_H);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = MEMLCD_W; s_drv.ver_res = MEMLCD_H;
    s_drv.flush_cb = flush_cb; s_drv.draw_buf = &s_draw_buf; s_drv.full_refresh = 1;
    s_disp = lv_disp_drv_register(&s_drv);
    lvgl_port_unlock();
    return s_disp != NULL;
}

static bool try_attach(void)
{
    if (s_attached) return true;
    if (memlcd_panel_init() != ESP_OK) return false;   /* bus pas encore là : réessai au tick suivant */
    s_attached = true;
    memlcd_panel_clear();
    ESP_LOGI(TAG, "panneau attache (bus radio pret)");
    s_dirty = true;                                    /* pousser ce que LVGL a déjà rendu */
    return true;
}

/* ── Vtable ───────────────────────────────────────────────────────── */
static bool memlcd_init(void)
{
    if (!lvgl_pret()) return false;
    try_attach();              /* réussit si la radio est déjà initialisée (droite) */
    return true;               /* jamais « KO » : l'attachement se fera au premier update() */
}

static void memlcd_refresh_all(void)
{
    if (!s_disp || !lvgl_port_lock(200)) return;
    construire();
    lire_modele(&s_shown);
    dessiner(&s_shown);
    lvgl_port_unlock();
}

static void memlcd_update(void)
{
    if (!s_disp) return;
    try_attach();
    if (s_sleeping) return;
    if (!s_built) { memlcd_refresh_all(); return; }
    memlcd_model_t m; lire_modele(&m);
    if (memlcd_model_diff(&s_shown, &m)) {
        if (lvgl_port_lock(50)) { s_shown = m; dessiner(&m); lvgl_port_unlock(); }
    }
    if (s_dirty && s_attached) {
        if (memlcd_panel_show(s_fb)) s_dirty = false;
        return;
    }
    /* Entretien VCOM ~1 Hz quand rien ne s'écrit (update() toutes les ~100 ms). */
    static uint8_t n; if (++n >= 10) { n = 0; memlcd_panel_vcom_tick(); }
}

static void memlcd_sleep(void) { s_sleeping = true; }            /* image gelée, plus de VCOM */
static void memlcd_wake(void)
{
    s_sleeping = false;
    s_dirty = true;                                                /* l'image revit tout de suite */
    if (s_disp && lvgl_port_lock(50)) { lv_obj_invalidate(lv_scr_act()); lvgl_port_unlock(); }
}
static void memlcd_noop(void) {}
static void memlcd_show_dfu(void)
{
    if (!s_disp || !lvgl_port_lock(100)) return;
    lv_obj_clean(lv_scr_act()); s_built = false;
    lv_obj_t *l = texte(lv_scr_act(), &lv_font_montserrat_14, 0, 70);
    lv_obj_set_width(l, MEMLCD_W); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "DFU");
    lvgl_port_unlock();
}

const display_backend_t memlcd_display_backend = {
    .init = memlcd_init, .update_layer = memlcd_update, .update = memlcd_update,
    .refresh_all = memlcd_refresh_all, .sleep = memlcd_sleep, .wake = memlcd_wake,
    .notify_mouse = memlcd_noop, .notify_keypress = memlcd_noop,
    .notify_display_key = memlcd_noop, .show_dfu = memlcd_show_dfu,
};
