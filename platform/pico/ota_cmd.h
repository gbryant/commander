#pragma once
#include "core/Writer.h"
#include "pico_fota_bootloader/core.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Linker symbols from pfb's generated linker_definitions.ld.
// __FLASH_DOWNLOAD_SLOT_START: XIP address of download slot.
// __FLASH_SWAP_SPACE_LENGTH:   size of each slot in bytes.
// Accessed via (uint32_t)&sym == value.
extern "C" {
    extern uint32_t __FLASH_DOWNLOAD_SLOT_START;
    extern uint32_t __FLASH_SWAP_SPACE_LENGTH;
}

// Erase one 4 KB sector of the download slot then write 'len' bytes from buf.
// 'len' must be a multiple of 256 and <= FLASH_SECTOR_SIZE.
// Interrupts are disabled only for each individual flash operation:
//   sector erase ~100 ms, 256-byte page program ~5 ms.
static void ota_erase_and_write(uint32_t slot_offset, const uint8_t *buf, size_t len) {
    uint32_t raw = (uint32_t)&__FLASH_DOWNLOAD_SLOT_START - XIP_BASE + slot_offset;

    uint32_t irqs = save_and_disable_interrupts();
    flash_range_erase(raw, FLASH_SECTOR_SIZE);
    restore_interrupts(irqs);

    for (size_t i = 0; i < len / 256; i++) {
        irqs = save_and_disable_interrupts();
        flash_range_program(raw + i * 256, buf + i * 256, 256);
        restore_interrupts(irqs);
    }
}

static void cmdOta(const char *args, Writer &out, void *) {
    while (*args == ' ') args++;
    if (!*args) { out.writeln("usage: ota <url>"); return; }

    printf("[ota] start: %s\n", args);

    if (strncmp(args, "http://", 7) != 0) {
        printf("[ota] err: not http\n");
        out.writeln("err: only http://"); return;
    }
    const char *p = args + 7;

    // Parse host[:port]/path
    char host[64]; int port = 80; char path[128];
    const char *slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);

    size_t hlen = (size_t)(slash - p);
    const char *colon = (const char *)memchr(p, ':', hlen);
    if (colon) {
        size_t hn = (size_t)(colon - p);
        if (hn >= sizeof(host)) { out.writeln("err: hostname too long"); return; }
        memcpy(host, p, hn); host[hn] = '\0';
        port = atoi(colon + 1);
    } else {
        if (hlen >= sizeof(host)) { out.writeln("err: hostname too long"); return; }
        memcpy(host, p, hlen); host[hlen] = '\0';
    }
    if (*slash == '\0') {
        path[0] = '/'; path[1] = '\0';
    } else {
        if (strlen(slash) >= sizeof(path)) { out.writeln("err: path too long"); return; }
        strncpy(path, slash, sizeof(path) - 1); path[sizeof(path) - 1] = '\0';
    }

    printf("[ota] host=%s port=%d path=%s\n", host, port, path);

    // core1 runs the IR PIO loop. On RP2040, flash_range_erase/program require
    // core1 to be idle — executing flash code from core1 during a flash write
    // causes a hang. Reset core1 now; OTA reboots on completion so no restart needed.
    multicore_reset_core1();
    printf("[ota] core1 stopped\n");

    // Commit current firmware so pfb won't roll back if we reboot mid-OTA.
    pfb_firmware_commit();
    printf("[ota] firmware committed\n");

    // Open HTTP connection BEFORE touching flash — WiFi must be fully alive here.
    struct addrinfo hints = {}; struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);
    printf("[ota] dns lookup: %s\n", host);
    if (lwip_getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        printf("[ota] err: dns failed\n");
        out.writeln("err: dns"); return;
    }
    printf("[ota] dns ok\n");

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[ota] err: socket() failed\n");
        lwip_freeaddrinfo(res); out.writeln("err: socket"); return;
    }
    printf("[ota] connecting...\n");
    if (lwip_connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        printf("[ota] err: connect() failed\n");
        lwip_freeaddrinfo(res); lwip_close(sock); out.writeln("err: connect"); return;
    }
    lwip_freeaddrinfo(res);
    printf("[ota] connected\n");

    // HTTP/1.0 GET (Connection: close so server sends EOF after body)
    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    lwip_send(sock, req, (size_t)rlen, 0);
    printf("[ota] GET sent (%d bytes)\n", rlen);

    // Skip response headers — scan for \r\n\r\n via 4-byte shift register
    uint32_t tail = 0;
    while (tail != 0x0D0A0D0Au) {
        char c;
        if (lwip_recv(sock, &c, 1, 0) != 1) {
            printf("[ota] err: header recv failed\n");
            lwip_close(sock); out.writeln("err: header"); return;
        }
        tail = (tail << 8) | (uint8_t)c;
    }
    printf("[ota] headers done, downloading\n");

    // Download + erase + write one 4 KB sector at a time.
    //
    // Receive a full sector over the network (WiFi fully alive), then
    // erase+write that sector (~180 ms of brief IRQ blackouts), then
    // yield 10 ms so the CYW43 driver can process any queued events,
    // then receive the next sector. This keeps WiFi alive throughout.
    static uint8_t sec_buf[FLASH_SECTOR_SIZE];  // 4096 bytes, static = no stack pressure
    size_t total      = 0;
    uint32_t slot_off = 0;
    uint32_t slot_sz  = (uint32_t)&__FLASH_SWAP_SPACE_LENGTH;
    uint32_t n_sects  = 0;
    out.write("flashing");

    for (;;) {
        // Fill sec_buf with up to 4 KB from the network
        size_t filled = 0;
        bool eof = false;
        while (filled < FLASH_SECTOR_SIZE) {
            int n = lwip_recv(sock, (char *)sec_buf + filled, FLASH_SECTOR_SIZE - filled, 0);
            if (n < 0) {
                printf("[ota] err: recv() = %d at offset %u\n", n, (unsigned)slot_off);
                lwip_close(sock); out.writeln("\nerr: recv"); return;
            }
            if (n == 0) { eof = true; break; }
            filled += (size_t)n;
        }
        if (filled == 0) break;

        total += filled;

        if (slot_off + FLASH_SECTOR_SIZE > slot_sz) {
            printf("[ota] err: firmware too large (slot_off=%u slot_sz=%u)\n",
                   (unsigned)slot_off, (unsigned)slot_sz);
            lwip_close(sock); out.writeln("\nerr: overflow"); return;
        }

        // Pad last partial sector to 256-byte boundary with 0xFF
        size_t write_len = filled;
        if (write_len % 256 != 0) {
            size_t padded = ((write_len + 255) / 256) * 256;
            memset(sec_buf + write_len, 0xFF, padded - write_len);
            write_len = padded;
        }

        printf("[ota] sector %u: recv=%u write=%u total=%u\n",
               (unsigned)n_sects, (unsigned)filled, (unsigned)write_len, (unsigned)total);

        ota_erase_and_write(slot_off, sec_buf, write_len);

        // Yield so the CYW43 driver processes any events queued during the erase
        vTaskDelay(pdMS_TO_TICKS(10));

        printf("[ota] sector %u: flash ok\n", (unsigned)n_sects);

        slot_off += FLASH_SECTOR_SIZE;
        n_sects++;

        // Dot every 32 KB on the telnet side
        if (slot_off % (32 * 1024) == 0) out.write(".");

        if (eof) break;
    }
    lwip_close(sock);
    printf("[ota] download complete: %u bytes in %u sectors\n",
           (unsigned)total, (unsigned)n_sects);

    // pfb_firmware_sha256_check reads 'total' bytes from the download slot and
    // compares SHA256 of the first (total-256) bytes against the 32-byte digest
    // stored in the last 256-byte block. total must be a multiple of 256.
    printf("[ota] sha256 check (size=%u)...\n", (unsigned)total);
    if (pfb_firmware_sha256_check(total) != 0) {
        printf("[ota] err: sha256 FAILED\n");
        out.writeln("\nerr: sha256"); return;
    }
    printf("[ota] sha256 ok\n");

    pfb_mark_download_slot_as_valid();
    printf("[ota] slot marked valid, rebooting\n");

    char msg[48];
    snprintf(msg, sizeof(msg), "\nok %u bytes — rebooting", (unsigned)total);
    out.writeln(msg);
    vTaskDelay(pdMS_TO_TICKS(200));
    pfb_perform_update();
}
