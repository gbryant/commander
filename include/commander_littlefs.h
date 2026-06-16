#pragma once
// ESP-IDF / esp32 only. Convenience mount for a LittleFS data partition added by
// `cmdr enable littlefs`. Include from your app (which pulls joltwallet/esp_littlefs
// via main/idf_component.yml) and call once at startup.
#include "esp_littlefs.h"

// Mount the LittleFS partition `label` at `base_path` (format-on-fail, so a blank
// device comes up with an empty filesystem rather than wedging). Returns true on
// success. Example: commander_mount_littlefs("storage", "/storage").
static inline bool commander_mount_littlefs(const char *label, const char *base_path) {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = base_path;
    conf.partition_label        = label;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;
    return esp_vfs_littlefs_register(&conf) == ESP_OK;
}
