#pragma once
#include <Arduino.h>

struct BBoxCoords {
    int   x;
    int   y;
    int   w;
    int   h;
    int   target;
    float confidence;
};

void initDiscordAlert(const char* webhook_url, bool enabled);
void sendDiscordBootNotification(const char* pin, const char* local_ip, const char* stream_url);
void sendDiscordStressAlert(
    const char* emotion,
    float stress_index,
    float duration_sec,
    float confidence,
    int   css_level,
    const char* stream_url,
    const uint8_t* jpeg_buf,
    size_t jpeg_len,
    const BBoxCoords* bbox
);
