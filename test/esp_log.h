/* stub: esp_log.h — utilisé uniquement dans les builds hôte (TEST_HOST).
 * Remplace les macros ESP-IDF par des no-ops silencieux.
 * Placé dans test/ et résolu via le chemin d'inclusion CMake. */
#pragma once
#define ESP_LOGI(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while(0)
