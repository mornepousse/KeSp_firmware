/*
 * test/fuzz/ccid_host_stubs.h — host-side shim soup for compiling the
 * target-only TinyUSB CCID class driver (main/security/ccid.c) on the
 * fuzzing host.
 *
 * ccid.c includes <freertos/FreeRTOS.h>, <freertos/task.h>,
 * <freertos/semphr.h>, <device/usbd_pvt.h>, <esp_timer.h> and <esp_log.h>.
 * The thin stub headers under test/fuzz/ccid_stubs/ all funnel into this one
 * file (so the definitions live in a single place and pragma-once dedups).
 *
 * Design notes:
 *   - xSemaphoreGive(s_msg_ready) runs the extracted worker body SYNCHRONOUSLY
 *     via ccid_host_run_worker() (ccid.c, under CCID_HOST_FUZZ).  This is what
 *     lets the fuzzer exercise the dispatch -> worker -> openpgp_card_apdu path
 *     without a real FreeRTOS task.
 *   - usbd_defer_func() runs its callback synchronously too, so the response
 *     builders (ccid_send_final_cb -> usbd_edpt_xfer) are exercised as well.
 *   - usbd_edpt_xfer() asserts total_bytes <= 512 (CCID_BUF_SZ): a response
 *     length larger than the static buffer is a memory-safety finding.
 *   - configASSERT()/TU_ASSERT failures abort -> ASan/the fuzzer catches them.
 *
 * Real function implementations live in ccid_host_stubs.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

/* ================================================================== */
/* FreeRTOS shims                                                      */
/* ================================================================== */
typedef int      BaseType_t;
typedef uint32_t TickType_t;
typedef void    *TaskHandle_t;
typedef void    *SemaphoreHandle_t;
typedef void   (*TaskFunction_t)(void *);

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define portMAX_DELAY      ((TickType_t)0xffffffffUL)
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
#define configASSERT(x)    assert(x)

BaseType_t        ccid_stub_xTaskCreate(TaskFunction_t fn, const char *name,
                                        uint32_t stack, void *param,
                                        uint32_t prio, TaskHandle_t *out);
void              ccid_stub_vTaskDelay(TickType_t ticks);
uint32_t          ccid_stub_uxTaskGetStackHighWaterMark(TaskHandle_t t);
SemaphoreHandle_t ccid_stub_xSemaphoreCreateBinary(void);
BaseType_t        ccid_stub_xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);

/* ccid.c (CCID_HOST_FUZZ) defines this; the give shim calls it synchronously. */
void              ccid_host_run_worker(void);

static inline BaseType_t ccid_stub_xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    ccid_host_run_worker();
    return pdTRUE;
}

#define xTaskCreate(fn, name, stack, param, prio, out) \
        ccid_stub_xTaskCreate((TaskFunction_t)(fn), (name), (uint32_t)(stack), \
                              (param), (uint32_t)(prio), (out))
#define vTaskDelay(t)                    ccid_stub_vTaskDelay((TickType_t)(t))
#define uxTaskGetStackHighWaterMark(t)   ccid_stub_uxTaskGetStackHighWaterMark(t)
#define xSemaphoreCreateBinary()         ccid_stub_xSemaphoreCreateBinary()
#define xSemaphoreTake(s, t)             ccid_stub_xSemaphoreTake((s), (TickType_t)(t))
#define xSemaphoreGive(s)                ccid_stub_xSemaphoreGive(s)

/* ================================================================== */
/* esp_timer shim                                                      */
/* ================================================================== */
int64_t esp_timer_get_time(void);

/* ================================================================== */
/* esp_log shims — no-op, but reference the tag so `static const char  */
/* *TAG` does not warn as unused.                                      */
/* ================================================================== */
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))

/* ================================================================== */
/* TinyUSB / usbd_pvt shims                                            */
/* ================================================================== */
typedef enum {
    XFER_RESULT_SUCCESS = 0,
    XFER_RESULT_FAILED,
    XFER_RESULT_STALLED,
    XFER_RESULT_TIMEOUT,
} xfer_result_t;

#define TUSB_CLASS_SMART_CARD  0x0B
#define TUSB_DESC_ENDPOINT     0x05
#define TUSB_XFER_BULK         2

#define TU_VERIFY(cond, ret)   do { if (!(cond)) return (ret); } while (0)
#define TU_ASSERT(cond, ret)   do { if (!(cond)) return (ret); } while (0)

/* Standard 9-byte interface descriptor (only bInterfaceClass is read by
 * ccid_drv_open; the rest is here so the struct matches the real layout). */
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} tusb_desc_interface_t;

typedef struct { uint8_t _opaque; } tusb_control_request_t;

typedef struct {
    char const *name;
    void     (*init)(void);
    bool     (*deinit)(void);
    void     (*reset)(uint8_t rhport);
    uint16_t (*open)(uint8_t rhport, tusb_desc_interface_t const *desc_intf,
                     uint16_t max_len);
    bool     (*control_xfer_cb)(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request);
    bool     (*xfer_cb)(uint8_t rhport, uint8_t ep_addr,
                        xfer_result_t result, uint32_t xferred_bytes);
    bool     (*xfer_isr)(uint8_t rhport, uint8_t ep_addr,
                         xfer_result_t result, uint32_t xferred_bytes);
    void     (*sof)(uint8_t rhport, uint32_t frame_count);
} usbd_class_driver_t;

static inline uint8_t const *tu_desc_next(void const *desc)
{
    uint8_t const *d = (uint8_t const *)desc;
    return d + d[0];
}
static inline uint8_t tu_desc_type(void const *desc)
{
    return ((uint8_t const *)desc)[1];
}

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr,
                    uint8_t *buffer, uint16_t total_bytes);
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr);
void usbd_defer_func(void (*func)(void *), void *param, bool in_isr);
bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *p_desc,
                         uint8_t ep_count, uint8_t xfer_type,
                         uint8_t *ep_out, uint8_t *ep_in);

/* Instrumentation counters the fuzzer can inspect. */
extern uint16_t g_ccid_stub_xfer_max;    /* largest total_bytes ever queued   */
extern uint64_t g_ccid_stub_xfer_calls;  /* number of usbd_edpt_xfer() calls   */
