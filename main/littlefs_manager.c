
#include "littlefs_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_littlefs.h"

static const char *TAG_LittleFS = "esp_littlefs";

void littlefs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGI(TAG_LittleFS, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGI(TAG_LittleFS, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGI(TAG_LittleFS, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_LittleFS, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
    }
    else
    {
        ESP_LOGI(TAG_LittleFS, "Partition size: total: %d, used: %d", total, used);
    }

    // Use POSIX and C standard library functions to work with files.
    // First create a file.
    ESP_LOGI(TAG_LittleFS, "Opening file");
    FILE *f = fopen("/littlefs/config", "w");
    if (f == NULL)
    {
        ESP_LOGE(TAG_LittleFS, "Failed to open file for writing");
        return;
    }
}

char *get_file_content(char *filename)
{
    //ESP_LOGI(TAG_LittleFS, "Getting content from file %s", filename);
    FILE *f = fopen(filename, "r");
    if (f == NULL)
    {
      //  ESP_LOGE(TAG_LittleFS, "Failed to open file for reading");
        return NULL;
    }
    char line[128];
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char *pos = strchr(line, '\n');
    if (pos)
    {
        *pos = '\0';
    }
    //ESP_LOGI(TAG_LittleFS, "Read from file: '%s'", line);
    return strdup(line);
}

void write_file_content(char *filename, char *content)
{
    ESP_LOGI(TAG_LittleFS, "Writing to file %s", filename);
    FILE *f = fopen(filename, "w");
    if (f == NULL)
    {
        ESP_LOGE(TAG_LittleFS, "Failed to open file for writing");
        return;
    }
    fprintf(f, "%s\n", content);
    fclose(f);
    ESP_LOGI(TAG_LittleFS, "Wrote to file: '%s'", content);
}

void delete_file_content(char *filename)
{
    ESP_LOGI(TAG_LittleFS, "Deleting file %s", filename);
    if (remove(filename) != 0)
    {
        ESP_LOGE(TAG_LittleFS, "Failed to delete file");
    }
    else
    {
        ESP_LOGI(TAG_LittleFS, "File deleted successfully");
    }
}

char *get_config_file_content(void)
{
    return get_file_content("/littlefs/config");
}
