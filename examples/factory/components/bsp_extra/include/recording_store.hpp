/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdio>
#include <cstdint>
#include "esp_spiffs.h"

/* PCM scratch storage avoids a 320 KB recording buffer on ESP32-C6. */
class RecordingStore {
public:
    ~RecordingStore() { finish(); }
    bool beginWrite() {
        finish();
        if (!esp_spiffs_mounted("recording")) {
            const esp_vfs_spiffs_conf_t config = {
                .base_path = "/recording", .partition_label = "recording",
                .max_files = 1, .format_if_mount_failed = true,
            };
            if (esp_vfs_spiffs_register(&config) != ESP_OK) return false;
        }
        file_ = fopen("/recording/mic.pcm", "wb");
        if (file_) setvbuf(file_, buffer_, _IOFBF, sizeof(buffer_));
        return file_ != nullptr;
    }
    bool beginRead() {
        if (!finish()) return false;
        file_ = fopen("/recording/mic.pcm", "rb");
        if (file_) setvbuf(file_, buffer_, _IOFBF, sizeof(buffer_));
        return file_ != nullptr;
    }
    bool write(const int16_t *samples, size_t count) {
        return file_ && fwrite(samples, sizeof(int16_t), count, file_) == count;
    }
    bool read(int16_t *samples, size_t count) {
        return file_ && fread(samples, sizeof(int16_t), count, file_) == count;
    }
    bool finish() {
        if (!file_) return true;
        FILE *file = file_; file_ = nullptr;
        return fclose(file) == 0;
    }
private:
    FILE *file_ = nullptr;
    char buffer_[4096] = {};
};
