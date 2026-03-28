/**
 * @file tamagotchi.c
 * @brief Virtual pet companion implementation
 * 
 * ASCII art-based Tamagotchi that reacts to keyboard activity.
 */

#include "tamagotchi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "TAMAGOTCHI"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define TAMA_IDLE_TIMEOUT_MS        (30 * 1000)         /* 30 seconds */
#define TAMA_SLEEPY_TIMEOUT_MS      (2 * 60 * 1000)     /* 2 minutes */
#define TAMA_SLEEP_TIMEOUT_MS       (5 * 60 * 1000)     /* 5 minutes */
#define TAMA_TIRED_SESSION_MS       (30 * 60 * 1000)    /* 30 minutes */

#define TAMA_KPM_WORKING_MIN        10      /* Min KPM to be "working" */
#define TAMA_KPM_HAPPY_THRESHOLD    100     /* KPM for happy state */
#define TAMA_KPM_EXCITED_THRESHOLD  200     /* KPM for excited state */

#define TAMA_SAVE_INTERVAL_MS       (5 * 60 * 1000)     /* Save every 5 min */

#define TAMA_NVS_NAMESPACE          "tamagotchi"
#define TAMA_NVS_STATS_KEY          "stats"

/* Celebration milestones (keypresses) */
static const uint32_t milestones[] = {
    1000, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000
};
#define MILESTONE_COUNT (sizeof(milestones) / sizeof(milestones[0]))

/* ============================================================================
 * Face expressions (ASCII art)
 * ============================================================================ */

typedef struct {
    const char *face;       /* Main face expression */
    const char *extra;      /* Extra decoration (zzZ, sparkles, etc.) */
} tama_expression_t;

static const tama_expression_t expressions[TAMA_STATE_COUNT] = {
    [TAMA_STATE_IDLE]        = { .face = "(o . o)",   .extra = NULL },
    [TAMA_STATE_WORKING]     = { .face = "(o _ o)",   .extra = NULL },
    [TAMA_STATE_HAPPY]       = { .face = "(^ _ ^)",   .extra = NULL },
    [TAMA_STATE_EXCITED]     = { .face = "(* o *)",   .extra = "!!" },
    [TAMA_STATE_SLEEPY]      = { .face = "(- _ -)",   .extra = "..." },
    [TAMA_STATE_SLEEPING]    = { .face = "(- . -)",   .extra = "zzZ" },
    [TAMA_STATE_TIRED]       = { .face = "(o _ o)",   .extra = "..." },
    [TAMA_STATE_CELEBRATING] = { .face = "\\(^o^)/",  .extra = "!!" },
};

/* ============================================================================
 * State
 * ============================================================================ */

static struct {
    tama_state_t state;
    tama_stats_t stats;
    
    /* Timing */
    TickType_t last_keypress_tick;
    TickType_t session_start_tick;
    TickType_t last_save_tick;
    TickType_t celebration_start_tick;
    
    /* UI */
    lv_obj_t *container;
    lv_obj_t *face_label;
    lv_obj_t *extra_label;
    
    /* Animation */
    lv_anim_t breath_anim;
    bool anim_running;
    
    /* Flags */
    bool initialized;
    bool visible;
    bool stats_dirty;
    uint8_t next_milestone_idx;
} tama = {0};

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static void update_expression(void);
static void start_idle_animation(void);
static void stop_animation(void);
static void check_milestones(void);
static void load_stats_from_nvs(void);
static void save_stats_to_nvs(void);

/* ============================================================================
 * Animation callbacks
 * ============================================================================ */

static void breath_anim_cb(void *var, int32_t val)
{
    if (tama.container) {
        lv_obj_set_style_translate_y(tama.container, val, 0);
    }
}

static void start_idle_animation(void)
{
    if (tama.anim_running || !tama.container) return;
    
    lv_anim_init(&tama.breath_anim);
    lv_anim_set_var(&tama.breath_anim, tama.container);
    lv_anim_set_values(&tama.breath_anim, 0, 3);
    lv_anim_set_time(&tama.breath_anim, 1500);
    lv_anim_set_playback_time(&tama.breath_anim, 1500);
    lv_anim_set_repeat_count(&tama.breath_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&tama.breath_anim, breath_anim_cb);
    lv_anim_set_path_cb(&tama.breath_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&tama.breath_anim);
    
    tama.anim_running = true;
}

static void stop_animation(void)
{
    if (!tama.anim_running) return;
    
    lv_anim_del(tama.container, breath_anim_cb);
    if (tama.container) {
        lv_obj_set_style_translate_y(tama.container, 0, 0);
    }
    tama.anim_running = false;
}

/* ============================================================================
 * State machine
 * ============================================================================ */

static tama_state_t compute_state(uint32_t kpm)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t idle_time = (now - tama.last_keypress_tick) * portTICK_PERIOD_MS;
    TickType_t session_time = (now - tama.session_start_tick) * portTICK_PERIOD_MS;
    
    /* Check for celebration state (temporary) */
    if (tama.state == TAMA_STATE_CELEBRATING) {
        TickType_t celeb_time = (now - tama.celebration_start_tick) * portTICK_PERIOD_MS;
        if (celeb_time < 3000) {  /* Celebrate for 3 seconds */
            return TAMA_STATE_CELEBRATING;
        }
    }
    
    /* Sleep states based on inactivity */
    if (idle_time > TAMA_SLEEP_TIMEOUT_MS) {
        return TAMA_STATE_SLEEPING;
    }
    if (idle_time > TAMA_SLEEPY_TIMEOUT_MS) {
        return TAMA_STATE_SLEEPY;
    }
    if (idle_time > TAMA_IDLE_TIMEOUT_MS) {
        return TAMA_STATE_IDLE;
    }
    
    /* Tired from long session */
    if (session_time > TAMA_TIRED_SESSION_MS && kpm < TAMA_KPM_HAPPY_THRESHOLD) {
        return TAMA_STATE_TIRED;
    }
    
    /* Activity-based states */
    if (kpm >= TAMA_KPM_EXCITED_THRESHOLD) {
        return TAMA_STATE_EXCITED;
    }
    if (kpm >= TAMA_KPM_HAPPY_THRESHOLD) {
        return TAMA_STATE_HAPPY;
    }
    if (kpm >= TAMA_KPM_WORKING_MIN) {
        return TAMA_STATE_WORKING;
    }
    
    return TAMA_STATE_IDLE;
}

static void update_expression(void)
{
    if (!tama.face_label) return;
    
    const tama_expression_t *expr = &expressions[tama.state];
    
    lv_label_set_text(tama.face_label, expr->face);
    
    if (tama.extra_label) {
        if (expr->extra) {
            lv_label_set_text(tama.extra_label, expr->extra);
            lv_obj_clear_flag(tama.extra_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tama.extra_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    /* Start/stop breathing animation based on state */
    if (tama.state == TAMA_STATE_IDLE || tama.state == TAMA_STATE_SLEEPY) {
        start_idle_animation();
    } else {
        stop_animation();
    }
}

/* ============================================================================
 * Milestones
 * ============================================================================ */

static void check_milestones(void)
{
    if (tama.next_milestone_idx >= MILESTONE_COUNT) return;
    
    uint32_t next = milestones[tama.next_milestone_idx];
    if (tama.stats.total_keypresses >= next) {
        ESP_LOGI(TAG, "🎉 Milestone reached: %lu keypresses!", (unsigned long)next);
        tamagotchi_celebrate();
        tama.next_milestone_idx++;
        
        /* Level up! */
        if (tama.stats.level < 255) {
            tama.stats.level++;
        }
        tama.stats_dirty = true;
    }
}

/* Find which milestone we're at */
static void init_milestone_index(void)
{
    tama.next_milestone_idx = 0;
    for (int i = 0; i < MILESTONE_COUNT; i++) {
        if (tama.stats.total_keypresses >= milestones[i]) {
            tama.next_milestone_idx = i + 1;
        } else {
            break;
        }
    }
}

/* ============================================================================
 * NVS persistence
 * ============================================================================ */

static void load_stats_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TAMA_NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (err == ESP_OK) {
        size_t size = sizeof(tama_stats_t);
        err = nvs_get_blob(handle, TAMA_NVS_STATS_KEY, &tama.stats, &size);
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded stats: total=%lu, level=%d", 
                     (unsigned long)tama.stats.total_keypresses, tama.stats.level);
        } else {
            ESP_LOGI(TAG, "No saved stats, starting fresh");
            memset(&tama.stats, 0, sizeof(tama_stats_t));
            tama.stats.happiness = 500;
            tama.stats.energy = 500;
        }
        
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        memset(&tama.stats, 0, sizeof(tama_stats_t));
        tama.stats.happiness = 500;
        tama.stats.energy = 500;
    }
    
    /* New session */
    tama.stats.session_keypresses = 0;
    tama.stats.sessions_count++;
    tama.stats_dirty = true;
}

static void save_stats_to_nvs(void)
{
    if (!tama.stats_dirty) return;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TAMA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, TAMA_NVS_STATS_KEY, &tama.stats, sizeof(tama_stats_t));
        if (err == ESP_OK) {
            nvs_commit(handle);
            tama.stats_dirty = false;
            ESP_LOGI(TAG, "Stats saved");
        }
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "Failed to save stats: %s", esp_err_to_name(err));
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void tamagotchi_init(void)
{
    if (tama.initialized) return;
    
    ESP_LOGI(TAG, "Initializing Tamagotchi 🐱");
    
    memset(&tama, 0, sizeof(tama));
    
    tama.state = TAMA_STATE_IDLE;
    tama.last_keypress_tick = xTaskGetTickCount();
    tama.session_start_tick = xTaskGetTickCount();
    tama.last_save_tick = xTaskGetTickCount();
    tama.visible = true;
    
    load_stats_from_nvs();
    init_milestone_index();
    
    tama.initialized = true;
    
    ESP_LOGI(TAG, "Tamagotchi ready! Level %d, %lu total keypresses",
             tama.stats.level, (unsigned long)tama.stats.total_keypresses);
}

void tamagotchi_draw(lv_obj_t *parent)
{
    if (!parent) return;
    
    /* Main face label - directly on parent, no container */
    tama.face_label = lv_label_create(parent);
    lv_obj_set_style_text_font(tama.face_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(tama.face_label, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_text_align(tama.face_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tama.face_label, LV_ALIGN_CENTER, 0, -10);
    
    /* Extra decoration label (sparkles, zzZ, etc.) */
    tama.extra_label = lv_label_create(parent);
    lv_obj_set_style_text_font(tama.extra_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tama.extra_label, lv_color_hex(0x58A6FF), 0);
    lv_obj_align(tama.extra_label, LV_ALIGN_CENTER, 55, -20);
    lv_obj_add_flag(tama.extra_label, LV_OBJ_FLAG_HIDDEN);
    
    /* Store face_label as container for animations */
    tama.container = tama.face_label;
    
    /* Initial expression */
    update_expression();
    
    ESP_LOGI(TAG, "Tamagotchi UI created");
}

void tamagotchi_update(uint32_t kpm)
{
    if (!tama.initialized) return;
    
    /* Update max KPM record */
    if (kpm > tama.stats.max_kpm_ever) {
        tama.stats.max_kpm_ever = kpm;
        tama.stats_dirty = true;
    }
    
    /* Compute new state */
    tama_state_t new_state = compute_state(kpm);
    
    if (new_state != tama.state) {
        ESP_LOGD(TAG, "State: %d -> %d (KPM=%lu)", tama.state, new_state, (unsigned long)kpm);
        tama.state = new_state;
        update_expression();
    }
    
    /* Periodic save */
    TickType_t now = xTaskGetTickCount();
    if ((now - tama.last_save_tick) * portTICK_PERIOD_MS > TAMA_SAVE_INTERVAL_MS) {
        save_stats_to_nvs();
        tama.last_save_tick = now;
    }
}

void tamagotchi_notify_keypress(void)
{
    if (!tama.initialized) return;
    
    tama.last_keypress_tick = xTaskGetTickCount();
    tama.stats.total_keypresses++;
    tama.stats.session_keypresses++;
    tama.stats_dirty = true;
    
    /* Check for milestones */
    check_milestones();
}

tama_state_t tamagotchi_get_state(void)
{
    return tama.state;
}

const tama_stats_t* tamagotchi_get_stats(void)
{
    return &tama.stats;
}

void tamagotchi_celebrate(void)
{
    tama.state = TAMA_STATE_CELEBRATING;
    tama.celebration_start_tick = xTaskGetTickCount();
    update_expression();
}

void tamagotchi_save_stats(void)
{
    tama.stats_dirty = true;  /* Force save */
    save_stats_to_nvs();
}

bool tamagotchi_is_visible(void)
{
    return tama.visible;
}

void tamagotchi_set_visible(bool visible)
{
    tama.visible = visible;
    if (tama.container) {
        if (visible) {
            lv_obj_clear_flag(tama.container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tama.container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
