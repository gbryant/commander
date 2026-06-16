#pragma once
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/Writer.h"
#include <stdio.h>

// OTA lifecycle hooks — weak no-ops so apps can override (e.g. to show progress on
// a display). begin() fires once flashing actually starts (with the image size, or
// -1 if the server sent no Content-Length); progress() after each chunk; end(ok)
// on failure (false) — on success the device reboots, so end(true) is informational
// and fires just before the restart. Every exit after begin() pairs with an end(),
// so an app that took over its display can always restore it.
extern "C" __attribute__((weak)) void commander_on_ota_begin(int total_bytes)         { (void)total_bytes; }
extern "C" __attribute__((weak)) void commander_on_ota_progress(int written, int tot) { (void)written; (void)tot; }
extern "C" __attribute__((weak)) void commander_on_ota_end(bool ok)                    { (void)ok; }

static void cmdOta(const char *args, Writer &out, void *) {
    while (*args == ' ') args++;
    if (!*args) { out.writeln("usage: ota <url>"); return; }

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) { out.writeln("err: no ota partition"); return; }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url        = args;
    http_cfg.timeout_ms = 30000;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        out.writeln("err: connect");
        return;
    }
    esp_http_client_fetch_headers(client);
    int expected = (int)esp_http_client_get_content_length(client);   // -1 if unknown

    esp_ota_handle_t ota;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        out.writeln("err: ota begin");
        return;
    }

    commander_on_ota_begin(expected);   // flashing starts — every exit below pairs with end()

    out.write("flashing");
    static char buf[512];
    int total = 0, n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            commander_on_ota_end(false);
            out.writeln("\nerr: flash write");
            return;
        }
        if ((total / 32768) < ((total + n) / 32768)) out.write(".");
        total += n;
        commander_on_ota_progress(total, expected);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (n < 0) { esp_ota_abort(ota); commander_on_ota_end(false); out.writeln("\nerr: download"); return; }
    if (esp_ota_end(ota) != ESP_OK) { commander_on_ota_end(false); out.writeln("\nerr: image invalid"); return; }
    if (esp_ota_set_boot_partition(target) != ESP_OK) { commander_on_ota_end(false); out.writeln("\nerr: set boot"); return; }

    commander_on_ota_end(true);
    char msg[48];
    snprintf(msg, sizeof(msg), "\nok %d bytes — rebooting", total);
    out.writeln(msg);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
