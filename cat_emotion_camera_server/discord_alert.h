#pragma once
#include <Arduino.h>

void initDiscordAlert(const char* webhook_url, bool enabled);
void sendDiscordBootNotification(const char* pin, const char* local_ip, const char* stream_url);
void sendDiscordStressAlert(const char* emotion, float stress_index, float duration_sec, float confidence, const char* stream_url);
