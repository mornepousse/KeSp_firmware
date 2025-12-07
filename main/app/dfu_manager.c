#include "soc/rtc_cntl_reg.h"
#include "esp_private/system_internal.h"

void reboot_to_dfu()
{
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}