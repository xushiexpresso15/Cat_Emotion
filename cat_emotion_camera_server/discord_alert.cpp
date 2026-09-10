#include "discord_alert.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

enum DiscordEventType {
    DISCORD_EVENT_BOOT = 0,
    DISCORD_EVENT_STRESS = 1
};

struct DiscordTaskMessage {
    DiscordEventType type;
    char param1[128]; // pin or emotion
    char param2[128]; // local_ip or duration
    char param3[128]; // stream_url or stress_index
    char param4[64];  // confidence
};

static QueueHandle_t s_discord_queue = nullptr;
static char s_webhook_url[256] = "";
static bool s_discord_enabled = false;

static void postJsonToDiscord(const char* json_payload) {
    if (!s_discord_enabled || strlen(s_webhook_url) < 10) {
        Serial.println("[Discord] Skipped: Webhook URL not configured or disabled.");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Discord] Cannot send notification: Wi-Fi not connected.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Fast handshake without bundle overhead
    client.setTimeout(5000);

    HTTPClient http;
    if (http.begin(client, s_webhook_url)) {
        http.addHeader("Content-Type", "application/json");
        http.setUserAgent("CatEmotionGateway/1.0 (ESP32-S3)");

        int http_code = http.POST((uint8_t*)json_payload, strlen(json_payload));
        if (http_code >= 200 && http_code < 300) {
            Serial.printf("[Discord] Message posted successfully (HTTP %d)\n", http_code);
        } else {
            Serial.printf("[Discord] Failed to post message (HTTP %d)\n", http_code);
        }
        http.end();
    } else {
        Serial.println("[Discord] HTTP begin failed.");
    }
}

static void discordWorkerTask(void* param) {
    DiscordTaskMessage msg;
    while (true) {
        if (xQueueReceive(s_discord_queue, &msg, portMAX_DELAY) == pdTRUE) {
            char payload[1024];

            if (msg.type == DISCORD_EVENT_BOOT) {
                // Boot notification
                snprintf(payload, sizeof(payload),
                    "{"
                    "\"username\":\"Cat Emotion Gateway\","
                    "\"embeds\":[{"
                        "\"title\":\"[SYSTEM] Cat Emotion Gateway Online\","
                        "\"description\":\"ESP32-S3 has booted and generated a new session PIN.\","
                        "\"color\":43690,"
                        "\"fields\":["
                            "{\"name\":\"Session PIN\",\"value\":\"`%s`\",\"inline\":true},"
                            "{\"name\":\"Local Status URL\",\"value\":\"http://%s/status\",\"inline\":true},"
                            "{\"name\":\"Cloud Live Stream\",\"value\":\"%s\",\"inline\":false}"
                        "],"
                        "\"footer\":{\"text\":\"Cat Emotion Monitoring System\"}"
                    "}]"
                    "}",
                    msg.param1, msg.param2, msg.param3
                );
                postJsonToDiscord(payload);
            } else if (msg.type == DISCORD_EVENT_STRESS) {
                // Stress anomaly alert
                snprintf(payload, sizeof(payload),
                    "{"
                    "\"username\":\"Cat Emotion Gateway\","
                    "\"embeds\":[{"
                        "\"title\":\"[ALERT] Feline Stress Anomaly Detected\","
                        "\"description\":\"Prolonged negative emotional posture detected exceeding the Cat Stress Score (CSS) threshold.\","
                        "\"color\":15158332,"
                        "\"fields\":["
                            "{\"name\":\"Dominant State\",\"value\":\"%s\",\"inline\":true},"
                            "{\"name\":\"Stress Index\",\"value\":\"%s%%\",\"inline\":true},"
                            "{\"name\":\"Sustained Duration\",\"value\":\"%ss\",\"inline\":true},"
                            "{\"name\":\"Average Confidence\",\"value\":\"%s%%\",\"inline\":true},"
                            "{\"name\":\"Live Stream Monitor\",\"value\":\"%s\",\"inline\":false},"
                            "{\"name\":\"Recommendation\",\"value\":\"Please check for environmental stressors, loud noises, or provide a safe hiding space.\",\"inline\":false}"
                        "],"
                        "\"footer\":{\"text\":\"Kessler & Turner Cat Stress Score (CSS) Algorithm\"}"
                    "}]"
                    "}",
                    msg.param1, msg.param3, msg.param2, msg.param4, "https://cat-emo-live.onrender.com"
                );
                postJsonToDiscord(payload);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void initDiscordAlert(const char* webhook_url, bool enabled) {
    s_discord_enabled = enabled;
    if (webhook_url && strlen(webhook_url) > 0) {
        strncpy(s_webhook_url, webhook_url, sizeof(s_webhook_url) - 1);
        s_webhook_url[sizeof(s_webhook_url) - 1] = '\0';
    }

    if (!s_discord_queue) {
        s_discord_queue = xQueueCreate(4, sizeof(DiscordTaskMessage));
        xTaskCreatePinnedToCore(
            discordWorkerTask,
            "discordTask",
            8192,
            nullptr,
            1, // Low priority
            nullptr,
            0  // Core 0
        );
        Serial.printf("[Discord] Worker task initialized. Alerts enabled: %s\n", s_discord_enabled ? "YES" : "NO");
    }
}

void sendDiscordBootNotification(const char* pin, const char* local_ip, const char* stream_url) {
    if (!s_discord_queue || !s_discord_enabled) return;

    DiscordTaskMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = DISCORD_EVENT_BOOT;
    strncpy(msg.param1, pin ? pin : "000000", sizeof(msg.param1) - 1);
    strncpy(msg.param2, local_ip ? local_ip : "192.168.4.1", sizeof(msg.param2) - 1);
    strncpy(msg.param3, stream_url ? stream_url : "https://cat-emo-live.onrender.com", sizeof(msg.param3) - 1);

    xQueueSend(s_discord_queue, &msg, 0);
}

void sendDiscordStressAlert(const char* emotion, float stress_index, float duration_sec, float confidence, const char* stream_url) {
    if (!s_discord_queue || !s_discord_enabled) return;

    DiscordTaskMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = DISCORD_EVENT_STRESS;
    strncpy(msg.param1, emotion ? emotion : "Unknown", sizeof(msg.param1) - 1);
    snprintf(msg.param2, sizeof(msg.param2), "%.1f", duration_sec);
    snprintf(msg.param3, sizeof(msg.param3), "%.0f", stress_index * 100.0f);
    snprintf(msg.param4, sizeof(msg.param4), "%.0f", confidence * 100.0f);

    xQueueSend(s_discord_queue, &msg, 0);
}
