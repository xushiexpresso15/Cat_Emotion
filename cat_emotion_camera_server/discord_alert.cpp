#include "discord_alert.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "jpeg_decoder.h"
#include <esp_heap_caps.h>
#include <ctype.h>

// ============================================================================
// Compact 5x7 ASCII Bitmap Font for On-Device Image Annotation
// Covers ASCII 32 (' ') through 90 ('Z')
// ============================================================================
static const uint8_t FONT_5X7_BLANK[5] = {0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t FONT_5X7_GLYPHS[][5] = {
    // 32: Space
    {0x00, 0x00, 0x00, 0x00, 0x00},
    // 33: !
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    // 34: "
    {0x00, 0x07, 0x00, 0x07, 0x00},
    // 35: #
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    // 36: $
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    // 37: %
    {0x23, 0x13, 0x08, 0x64, 0x62},
    // 38: &
    {0x36, 0x49, 0x55, 0x22, 0x50},
    // 39: '
    {0x00, 0x05, 0x03, 0x00, 0x00},
    // 40: (
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    // 41: )
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    // 42: *
    {0x14, 0x08, 0x3E, 0x08, 0x14},
    // 43: +
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    // 44: ,
    {0x00, 0x50, 0x30, 0x00, 0x00},
    // 45: -
    {0x08, 0x08, 0x08, 0x08, 0x08},
    // 46: .
    {0x00, 0x60, 0x60, 0x00, 0x00},
    // 47: /
    {0x20, 0x10, 0x08, 0x04, 0x02},
    // 48-57: 0 to 9
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    // 58: :
    {0x00, 0x36, 0x36, 0x00, 0x00},
    // 59: ;
    {0x00, 0x56, 0x36, 0x00, 0x00},
    // 60: <
    {0x08, 0x14, 0x22, 0x41, 0x00},
    // 61: =
    {0x14, 0x14, 0x14, 0x14, 0x14},
    // 62: >
    {0x00, 0x41, 0x22, 0x14, 0x08},
    // 63: ?
    {0x02, 0x01, 0x51, 0x09, 0x06},
    // 64: @
    {0x32, 0x49, 0x79, 0x41, 0x3E},
    // 65-90: A to Z
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}  // Z
};

static const uint8_t* getFontGlyph(char c) {
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    if (c >= 32 && c <= 90) {
        return FONT_5X7_GLYPHS[c - 32];
    }
    return FONT_5X7_BLANK;
}

static inline void setPixelRGB(uint8_t* rgb, int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < w && y >= 0 && y < h) {
        size_t idx = ((size_t)y * w + x) * 3;
        rgb[idx]     = r;
        rgb[idx + 1] = g;
        rgb[idx + 2] = b;
    }
}

static void fillRectRGB(uint8_t* rgb, int img_w, int img_h, int rx, int ry, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
    int x0 = max(0, rx);
    int y0 = max(0, ry);
    int x1 = min(img_w, rx + rw);
    int y1 = min(img_h, ry + rh);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            setPixelRGB(rgb, img_w, img_h, x, y, r, g, b);
        }
    }
}

static void drawBoxOutlineRGB(uint8_t* rgb, int img_w, int img_h, int rx, int ry, int rw, int rh, int thickness, uint8_t r, uint8_t g, uint8_t b) {
    fillRectRGB(rgb, img_w, img_h, rx, ry, rw, thickness, r, g, b);
    fillRectRGB(rgb, img_w, img_h, rx, ry + rh - thickness, rw, thickness, r, g, b);
    fillRectRGB(rgb, img_w, img_h, rx, ry, thickness, rh, r, g, b);
    fillRectRGB(rgb, img_w, img_h, rx + rw - thickness, ry, thickness, rh, r, g, b);
}

static void drawChar5x7(uint8_t* rgb, int img_w, int img_h, int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t* glyph = getFontGlyph(c);
    for (int col = 0; col < 5; ++col) {
        uint8_t col_bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (col_bits & (1 << row)) {
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        setPixelRGB(rgb, img_w, img_h, x + col * scale + dx, y + row * scale + dy, r, g, b);
                    }
                }
            }
        }
    }
}

static void drawString5x7(uint8_t* rgb, int img_w, int img_h, int x, int y, const char* str, int scale, uint8_t r, uint8_t g, uint8_t b) {
    int cur_x = x;
    while (*str) {
        drawChar5x7(rgb, img_w, img_h, cur_x, y, *str, scale, r, g, b);
        cur_x += (5 + 1) * scale;
        str++;
    }
}

// ============================================================================
// JPEG Decoding, Bounding Box Drawing & Re-encoding
// ============================================================================
static uint8_t* drawBoundingBoxOnJpeg(
    const uint8_t* jpeg_in,
    size_t jpeg_in_len,
    const BBoxCoords& bbox,
    const char* emotion,
    float confidence_pct,
    size_t* out_len
) {
    if (!jpeg_in || jpeg_in_len == 0 || !out_len) return nullptr;
    *out_len = 0;

    esp_jpeg_image_cfg_t info_cfg;
    memset(&info_cfg, 0, sizeof(info_cfg));
    info_cfg.indata = (uint8_t*)jpeg_in;
    info_cfg.indata_size = jpeg_in_len;
    info_cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    info_cfg.out_scale = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t img_info;
    memset(&img_info, 0, sizeof(img_info));
    if (esp_jpeg_get_image_info(&info_cfg, &img_info) != ESP_OK) {
        Serial.println("[Discord-Img] Failed to parse JPEG header info.");
        return nullptr;
    }

    uint8_t* rgb_buf = (uint8_t*)heap_caps_malloc(img_info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buf) {
        Serial.printf("[Discord-Img] Failed to allocate %u bytes in PSRAM for RGB888\n", (unsigned int)img_info.output_len);
        return nullptr;
    }

    esp_jpeg_image_cfg_t dec_cfg;
    memset(&dec_cfg, 0, sizeof(dec_cfg));
    dec_cfg.indata = (uint8_t*)jpeg_in;
    dec_cfg.indata_size = jpeg_in_len;
    dec_cfg.outbuf = rgb_buf;
    dec_cfg.outbuf_size = img_info.output_len;
    dec_cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    dec_cfg.out_scale = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t dec_out;
    memset(&dec_out, 0, sizeof(dec_out));
    if (esp_jpeg_decode(&dec_cfg, &dec_out) != ESP_OK) {
        Serial.println("[Discord-Img] Failed to decode JPEG to RGB888.");
        free(rgb_buf);
        return nullptr;
    }

    int img_w = dec_out.width;
    int img_h = dec_out.height;

    int bx = bbox.x;
    int by = bbox.y;
    int bw = bbox.w;
    int bh = bbox.h;

    // Safety fallback if bbox coordinates are not bounded
    if (bw <= 0 || bh <= 0) {
        bw = (img_w * 6) / 10;
        bh = (img_h * 6) / 10;
        bx = (img_w - bw) / 2;
        by = (img_h - bh) / 2;
    }

    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (bx >= img_w) bx = 0;
    if (by >= img_h) by = 0;
    if (bx + bw > img_w) bw = img_w - bx;
    if (by + bh > img_h) bh = img_h - by;

    // High-visibility palette:
    // Angry: Vivid Crimson (255, 35, 35)
    // Scared: Intense Amber-Orange (255, 130, 0)
    // Focus: Azure Blue (0, 180, 255)
    // Relax: Spring Green (40, 220, 60)
    uint8_t box_r = 255, box_g = 130, box_b = 0;
    if (strcasecmp(emotion, "Angry") == 0) {
        box_r = 255; box_g = 35; box_b = 35;
    } else if (strcasecmp(emotion, "Scared") == 0) {
        box_r = 255; box_g = 130; box_b = 0;
    } else if (strcasecmp(emotion, "Focus") == 0) {
        box_r = 0; box_g = 180; box_b = 255;
    }

    // 1. Draw 3-pixel thick bounding box outline
    drawBoxOutlineRGB(rgb_buf, img_w, img_h, bx, by, bw, bh, 3, box_r, box_g, box_b);

    // 2. Format banner label (e.g. "SCARED 92%")
    char label[32];
    snprintf(label, sizeof(label), "%s %.0f%%", emotion, confidence_pct);
    for (int i = 0; label[i]; ++i) {
        label[i] = toupper((unsigned char)label[i]);
    }

    int label_len = strlen(label);
    int font_scale = (img_w >= 400) ? 2 : 1;
    int char_w = 6 * font_scale;
    int char_h = 7 * font_scale;
    int banner_w = label_len * char_w + 8;
    int banner_h = char_h + 6;

    int banner_x = bx;
    int banner_y = (by >= banner_h) ? (by - banner_h) : (by + 3);
    if (banner_x + banner_w > img_w) {
        banner_x = img_w - banner_w;
    }
    if (banner_x < 0) banner_x = 0;

    // Draw banner badge background
    fillRectRGB(rgb_buf, img_w, img_h, banner_x, banner_y, banner_w, banner_h, box_r, box_g, box_b);

    // Draw crisp white label text
    drawString5x7(rgb_buf, img_w, img_h, banner_x + 4, banner_y + 3, label, font_scale, 255, 255, 255);

    // 3. Compress modified RGB888 buffer back to JPEG
    uint8_t* out_jpg = nullptr;
    size_t out_jpg_len = 0;
    bool ok = fmt2jpg(rgb_buf, dec_out.output_len, img_w, img_h, PIXFORMAT_RGB888, 80, &out_jpg, &out_jpg_len);

    free(rgb_buf);

    if (!ok || !out_jpg || out_jpg_len == 0) {
        Serial.println("[Discord-Img] fmt2jpg re-encoding failed.");
        if (out_jpg) free(out_jpg);
        return nullptr;
    }

    *out_len = out_jpg_len;
    return out_jpg;
}

// ============================================================================
// FreeRTOS Task and Notification Structures
// ============================================================================
enum DiscordEventType {
    DISCORD_EVENT_BOOT = 0,
    DISCORD_EVENT_STRESS = 1
};

struct DiscordTaskMessage {
    DiscordEventType type;
    char       param1[64];  // pin or emotion
    char       param2[64];  // local_ip or duration
    char       param3[128]; // stream_url or stress_index
    char       param4[32];  // confidence
    int        css_level;
    uint8_t*   jpeg_data;
    size_t     jpeg_len;
    BBoxCoords bbox;
    bool       has_bbox;
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
    client.setInsecure();
    client.setTimeout(6000);

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

static bool postMultipartToDiscord(const char* json_payload, const uint8_t* jpeg_data, size_t jpeg_len) {
    if (!s_discord_enabled || strlen(s_webhook_url) < 10) {
        Serial.println("[Discord] Skipped: Webhook URL not configured or disabled.");
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Discord] Cannot send notification: Wi-Fi not connected.");
        return false;
    }

    if (!jpeg_data || jpeg_len == 0) {
        postJsonToDiscord(json_payload);
        return true;
    }

    static const char BOUNDARY[] = "---------------------------esp32boundary987654321";

    char part1[256];
    int part1_len = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"payload_json\"\r\n"
        "Content-Type: application/json\r\n\r\n",
        BOUNDARY
    );

    int json_len = strlen(json_payload);

    char part2[256];
    int part2_len = snprintf(part2, sizeof(part2),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"snapshot.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        BOUNDARY
    );

    char part3[128];
    int part3_len = snprintf(part3, sizeof(part3),
        "\r\n--%s--\r\n",
        BOUNDARY
    );

    size_t total_body_len = part1_len + json_len + part2_len + jpeg_len + part3_len;

    uint8_t* body = (uint8_t*)heap_caps_malloc(total_body_len + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = (uint8_t*)malloc(total_body_len + 32);
    }

    if (!body) {
        Serial.println("[Discord] Memory allocation for multipart body failed; falling back to JSON embed.");
        postJsonToDiscord(json_payload);
        return false;
    }

    size_t offset = 0;
    memcpy(body + offset, part1, part1_len); offset += part1_len;
    memcpy(body + offset, json_payload, json_len); offset += json_len;
    memcpy(body + offset, part2, part2_len); offset += part2_len;
    memcpy(body + offset, jpeg_data, jpeg_len); offset += jpeg_len;
    memcpy(body + offset, part3, part3_len); offset += part3_len;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    HTTPClient http;
    bool success = false;
    if (http.begin(client, s_webhook_url)) {
        char ct_hdr[128];
        snprintf(ct_hdr, sizeof(ct_hdr), "multipart/form-data; boundary=%s", BOUNDARY);
        http.addHeader("Content-Type", ct_hdr);
        http.setUserAgent("CatEmotionGateway/1.0 (ESP32-S3)");

        int http_code = http.POST(body, offset);
        if (http_code >= 200 && http_code < 300) {
            Serial.printf("[Discord] Multipart snapshot message posted successfully (HTTP %d)\n", http_code);
            success = true;
        } else {
            Serial.printf("[Discord] Failed to post multipart message (HTTP %d)\n", http_code);
        }
        http.end();
    } else {
        Serial.println("[Discord] HTTP begin failed for multipart upload.");
    }

    free(body);
    return success;
}

static void discordWorkerTask(void* param) {
    DiscordTaskMessage msg;
    while (true) {
        if (xQueueReceive(s_discord_queue, &msg, portMAX_DELAY) == pdTRUE) {
            char payload[2048];

            if (msg.type == DISCORD_EVENT_BOOT) {
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
                        "\"footer\":{\"text\":\"Cat Emotion Monitoring System (ESP32-S3 Edge AI)\"}"
                    "}]"
                    "}",
                    msg.param1, msg.param2, msg.param3
                );
                postJsonToDiscord(payload);
            } else if (msg.type == DISCORD_EVENT_STRESS) {
                int embed_color = (strcasecmp(msg.param1, "Angry") == 0) ? 15158332 : 15105570;

                const char* css_desc = "CSS Level 6: Distressed / Scared";
                switch (msg.css_level) {
                    case 4: css_desc = "CSS Level 4: Very Tense"; break;
                    case 5: css_desc = "CSS Level 5: Anxious (High Vigilance)"; break;
                    case 6: css_desc = "CSS Level 6: Distressed / Scared"; break;
                    case 7: css_desc = "CSS Level 7: Terrified / Agonistic Defense"; break;
                    default: css_desc = "CSS Level 5-6 (Acute Distress)"; break;
                }

                uint8_t* final_jpeg = msg.jpeg_data;
                size_t   final_jpeg_len = msg.jpeg_len;
                uint8_t* annotated_jpeg = nullptr;
                size_t   annotated_len = 0;

                if (msg.jpeg_data != nullptr && msg.jpeg_len > 0) {
                    annotated_jpeg = drawBoundingBoxOnJpeg(
                        msg.jpeg_data,
                        msg.jpeg_len,
                        msg.bbox,
                        msg.param1,
                        atof(msg.param4),
                        &annotated_len
                    );
                    if (annotated_jpeg != nullptr && annotated_len > 0) {
                        final_jpeg = annotated_jpeg;
                        final_jpeg_len = annotated_len;
                    }
                }

                if (final_jpeg != nullptr && final_jpeg_len > 0) {
                    snprintf(payload, sizeof(payload),
                        "{"
                        "\"username\":\"Cat Emotion Gateway\","
                        "\"embeds\":[{"
                            "\"title\":\"[ALERT] Feline High Stress Anomaly Detected\","
                            "\"description\":\"Prolonged distress posture detected exceeding the Cat Stress Score (CSS) threshold.\","
                            "\"color\":%d,"
                            "\"fields\":["
                                "{\"name\":\"Dominant State\",\"value\":\"**%s**\",\"inline\":true},"
                                "{\"name\":\"CSS Assessment\",\"value\":\"`%s`\",\"inline\":true},"
                                "{\"name\":\"Stress Index\",\"value\":\"%s%%\",\"inline\":true},"
                                "{\"name\":\"Sustained Duration\",\"value\":\"%ss\",\"inline\":true},"
                                "{\"name\":\"Model Confidence\",\"value\":\"%s%%\",\"inline\":true},"
                                "{\"name\":\"Facial Action Markers\",\"value\":\"Ear flattening, orbital squint (AU105/AU43)\",\"inline\":true},"
                                "{\"name\":\"Scientific References\",\"value\":\"• [Kessler & Turner (1997) Animal Welfare](https://www.aspcapro.org/resource/cat-stress-score-css)\\n• [Evangelista et al. (2019) Nature Sci Rep](https://www.nature.com/articles/s41598-019-55693-8)\\n• [Stella et al. (2013) J Feline Med](https://journals.sagepub.com/doi/10.1177/1098612X13489215)\",\"inline\":false},"
                                "{\"name\":\"Caregiver Guidance\",\"value\":\"Check for environmental stressors, loud sounds, or provide a quiet retreat space.\",\"inline\":false},"
                                "{\"name\":\"Live Video Monitor\",\"value\":\"https://cat-emo-live.onrender.com\",\"inline\":false}"
                            "],"
                            "\"image\":{\"url\":\"attachment://snapshot.jpg\"},"
                            "\"footer\":{\"text\":\"Feline Behavioral & Stress Monitoring Sentinel (ESP32-S3 Edge AI)\"}"
                        "}]"
                        "}",
                        embed_color, msg.param1, css_desc, msg.param3, msg.param2, msg.param4
                    );
                    postMultipartToDiscord(payload, final_jpeg, final_jpeg_len);
                } else {
                    snprintf(payload, sizeof(payload),
                        "{"
                        "\"username\":\"Cat Emotion Gateway\","
                        "\"embeds\":[{"
                            "\"title\":\"[ALERT] Feline High Stress Anomaly Detected\","
                            "\"description\":\"Prolonged distress posture detected exceeding the Cat Stress Score (CSS) threshold.\","
                            "\"color\":%d,"
                            "\"fields\":["
                                "{\"name\":\"Dominant State\",\"value\":\"**%s**\",\"inline\":true},"
                                "{\"name\":\"CSS Assessment\",\"value\":\"`%s`\",\"inline\":true},"
                                "{\"name\":\"Stress Index\",\"value\":\"%s%%\",\"inline\":true},"
                                "{\"name\":\"Sustained Duration\",\"value\":\"%ss\",\"inline\":true},"
                                "{\"name\":\"Model Confidence\",\"value\":\"%s%%\",\"inline\":true},"
                                "{\"name\":\"Scientific References\",\"value\":\"• [Kessler & Turner (1997) Animal Welfare](https://www.aspcapro.org/resource/cat-stress-score-css)\\n• [Evangelista et al. (2019) Nature Sci Rep](https://www.nature.com/articles/s41598-019-55693-8)\",\"inline\":false},"
                                "{\"name\":\"Live Video Monitor\",\"value\":\"https://cat-emo-live.onrender.com\",\"inline\":false}"
                            "],"
                            "\"footer\":{\"text\":\"Feline Behavioral & Stress Monitoring Sentinel (ESP32-S3 Edge AI)\"}"
                        "}]"
                        "}",
                        embed_color, msg.param1, css_desc, msg.param3, msg.param2, msg.param4
                    );
                    postJsonToDiscord(payload);
                }

                if (annotated_jpeg) {
                    free(annotated_jpeg);
                }
                if (msg.jpeg_data) {
                    free(msg.jpeg_data);
                }
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
            12288,
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
) {
    if (!s_discord_queue || !s_discord_enabled) return;

    DiscordTaskMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = DISCORD_EVENT_STRESS;
    strncpy(msg.param1, emotion ? emotion : "Unknown", sizeof(msg.param1) - 1);
    snprintf(msg.param2, sizeof(msg.param2), "%.1f", duration_sec);
    snprintf(msg.param3, sizeof(msg.param3), "%.0f", stress_index * 100.0f);
    snprintf(msg.param4, sizeof(msg.param4), "%.0f", confidence * 100.0f);
    msg.css_level = css_level;

    if (jpeg_buf != nullptr && jpeg_len > 0) {
        uint8_t* copy = (uint8_t*)heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) {
            copy = (uint8_t*)malloc(jpeg_len);
        }
        if (copy) {
            memcpy(copy, jpeg_buf, jpeg_len);
            msg.jpeg_data = copy;
            msg.jpeg_len = jpeg_len;
        }
    }

    if (bbox != nullptr) {
        msg.bbox = *bbox;
        msg.has_bbox = true;
    } else {
        msg.has_bbox = false;
    }

    if (xQueueSend(s_discord_queue, &msg, 0) != pdTRUE) {
        if (msg.jpeg_data) {
            free(msg.jpeg_data);
            msg.jpeg_data = nullptr;
        }
        Serial.println("[Discord] Warning: Queue full, dropped stress alert message.");
    }
}
