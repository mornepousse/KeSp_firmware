/**
 * @file tamagotchi.h
 * @brief Virtual pet companion that reacts to keyboard usage
 * 
 * A Tamagotchi-like creature that lives on the round display and
 * responds to typing activity, KPM, and inactivity periods.
 */

#ifndef TAMAGOTCHI_H
#define TAMAGOTCHI_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tamagotchi emotional/activity states
 */
typedef enum {
    TAMA_STATE_IDLE,        /**< No activity for a short while */
    TAMA_STATE_WORKING,     /**< Normal typing activity */
    TAMA_STATE_HAPPY,       /**< Good typing speed (KPM > 100) */
    TAMA_STATE_EXCITED,     /**< Fast typing (KPM > 200) */
    TAMA_STATE_SLEEPY,      /**< Getting drowsy (inactivity > 2min) */
    TAMA_STATE_SLEEPING,    /**< Zzz... (inactivity > 5min) */
    TAMA_STATE_TIRED,       /**< Long session (> 30min) */
    TAMA_STATE_CELEBRATING, /**< Milestone reached! */
    TAMA_STATE_COUNT
} tama_state_t;

/**
 * @brief Persistent stats for the Tamagotchi (saved to NVS)
 */
typedef struct {
    uint32_t total_keypresses;      /**< Total keypresses since creation */
    uint32_t session_keypresses;    /**< Keypresses this session */
    uint32_t max_kpm_ever;          /**< Personal best KPM */
    uint32_t sessions_count;        /**< Number of sessions */
    uint16_t happiness;             /**< Happiness level 0-1000 */
    uint16_t energy;                /**< Energy level 0-1000 */
    uint8_t level;                  /**< Pet level (for future evolution) */
    uint8_t reserved[3];            /**< Reserved for future use */
} tama_stats_t;

/**
 * @brief Initialize the Tamagotchi system
 * 
 * Loads stats from NVS if available, initializes state machine.
 */
void tamagotchi_init(void);

/**
 * @brief Create and draw the Tamagotchi on a parent container
 * 
 * @param parent LVGL parent object to draw on
 */
void tamagotchi_draw(lv_obj_t *parent);

/**
 * @brief Update Tamagotchi state based on current KPM
 * 
 * Should be called periodically (e.g., every second).
 * 
 * @param kpm Current keys-per-minute value
 */
void tamagotchi_update(uint32_t kpm);

/**
 * @brief Notify Tamagotchi of a keypress
 * 
 * Call this on every keypress to track activity and stats.
 */
void tamagotchi_notify_keypress(void);

/**
 * @brief Get the current Tamagotchi state
 * 
 * @return Current emotional/activity state
 */
tama_state_t tamagotchi_get_state(void);

/**
 * @brief Get the current stats
 * 
 * @return Pointer to stats structure (read-only)
 */
const tama_stats_t* tamagotchi_get_stats(void);

/**
 * @brief Trigger a celebration animation
 * 
 * Called when a milestone is reached.
 */
void tamagotchi_celebrate(void);

/**
 * @brief Save stats to NVS
 * 
 * Call periodically and on shutdown.
 */
void tamagotchi_save_stats(void);

/**
 * @brief Check if Tamagotchi UI is visible
 * 
 * @return true if drawn and visible
 */
bool tamagotchi_is_visible(void);

/**
 * @brief Show/hide the Tamagotchi
 * 
 * @param visible true to show, false to hide
 */
void tamagotchi_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif /* TAMAGOTCHI_H */
