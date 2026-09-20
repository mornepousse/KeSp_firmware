/* stub: esp_log.h — used only in host builds (TEST_HOST).
 * Replaces the ESP-IDF macros with silent no-ops.
 * Placed in test/ and resolved via the CMake include path. */
#pragma once
#define ESP_LOGI(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while(0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while(0)
