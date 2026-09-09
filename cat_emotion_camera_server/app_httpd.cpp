// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modified by nullptr, Seeed Technology Inc (c) 2024
//

#include "app_httpd.h"

#include <ArduinoJson.h>
#include <FreeRTOS.h>
#include <Seeed_Arduino_SSCMA.h>
#include <Wire.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <sdkconfig.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>

#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "BYTETracker.h"
#include "web_index.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
    #include <HardwareSerial.h>
    #include <esp32-hal-log.h>
#endif

#define RESULT_TIMEOUT_MS 3000
#define CMD_TIMEOUT_MS    3000

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define PTR_BUFFER_SIZE     8
    #define COM_BUFFER_SIZE     (1024 * 128)
    #define RSP_BUFFER_SIZE     (1024 * 196)
    #define JPG_BUFFER_SIZE     (1024 * 128)
    #define RST_BUFFER_SIZE     (1024 * 64)
    #define QRY_BUFFER_SIZE     (1024 * 16)
    #define CMD_BUFFER_SIZE     (1024 * 12)

    #define BYTE_TRACKER_ENABLED 0
#else
    #warning "Server may not work properly due to resource constraints..."
    #define PTR_BUFFER_SIZE     3
    #define COM_BUFFER_SIZE     (1024 * 32)
    #define RSP_BUFFER_SIZE     (1024 * 32)
    #define JPG_BUFFER_SIZE     (1024 * 32)
    #define RST_BUFFER_SIZE     (1024 * 4)
    #define QRY_BUFFER_SIZE     (1024 * 4)
    #define CMD_BUFFER_SIZE     (1024 * 4)

    #define BYTE_TRACKER_ENABLED 0
#endif

#define CMD_TAG_FMT_STR "HTTPD%.8X@"
#define CMD_TAG_SIZE    snprintf(NULL, 0, CMD_TAG_FMT_STR, 0)

#define MSG_IMAGE_KEY   "\"image\": "
#define MSG_COMMA_STR   ", "
#define MSG_QUOTE_STR   "\""
#define MSG_REPLY_STR   "\"type\": 0"
#define MSG_EVENT_STR   "\"type\": 1"
#define MSG_LOGGI_STR   "\"type\": 2"
#define MSG_TERMI_STR   "\r\n"

enum MsgType : uint16_t {
    MSG_TYPE_UNKNOWN = 0,
    MSG_TYPE_REPLY   = 0xff & (1 << 1),
    MSG_TYPE_EVENT   = 0xff & (1 << 2),
    MSG_TYPE_LOGGI   = 0xff & (1 << 3),
};

#define CMD_SAMPLE_STR "SAMPLE"
#define CMD_INVOKE_STR "INVOKE"

enum CmdType : uint16_t {
    CMD_TYPE_UNKNOWN = 0,
    CMD_TYPE_SAMPLE  = 0xff00 & (1 << 8),
    CMD_TYPE_INVOKE  = 0xff00 & (2 << 8),
    CMD_TYPE_SENSOR  = 0xff00 & (3 << 8),
};

struct PtrBuffer {
    struct Slot {
        size_t   id   = 0;
        uint16_t type = 0;
        void*    data = NULL;
        size_t   size = 0;
        timeval  timestamp;
    };

    SemaphoreHandle_t                 mutex;
    std::deque<std::shared_ptr<Slot>> slots;
    volatile size_t                   id    = 1;
    const size_t                      limit = PTR_BUFFER_SIZE;
};

struct StatInfo {
    size_t            last_frame_id = 0;
    timeval           last_frame_timestamp;
    SemaphoreHandle_t mutex;
};

PtrBuffer PB;
StatInfo  SI;
SSCMA     AI;
WebSocketsServer webSocket(81);
WebSocketsClient cloudClient;

void initCloudRelay(const char* host, uint16_t port, const char* path, bool ssl) {
    if (!host || strlen(host) == 0) return;
    static bool s_initialized = false;
    if (s_initialized) return;
    s_initialized = true;

    Serial.printf("[Cloud WS] 初始化雲端推流連線: %s%s:%d%s ...\n",
        ssl ? "wss://" : "ws://", host, port, path);
    if (ssl) {
        cloudClient.beginSSL(host, port, path);
    } else {
        cloudClient.begin(host, port, path);
    }
    cloudClient.setReconnectInterval(5000);
    cloudClient.enableHeartbeat(15000, 3000, 2);
    cloudClient.onEvent([](WStype_t type, uint8_t * payload, size_t length) {
        if (type == WStype_CONNECTED) {
            Serial.println("[Cloud WS] [OK] 雲端中繼站連線成功！開始即時推流");
        } else if (type == WStype_DISCONNECTED) {
            Serial.println("[Cloud WS] 與雲端中繼站中斷連線 (5秒後自動重試)");
        } else if (type == WStype_ERROR) {
            Serial.println("[Cloud WS] 雲端連線發生錯誤");
        }
    });
}

volatile size_t   g_total_frames      = 0;
volatile size_t   g_last_frame_bytes  = 0;
volatile uint32_t g_last_frame_millis = 0;

void initSharedBuffer() { PB.mutex = xSemaphoreCreateMutex(); }

void initStatInfo() {
    SI.mutex                        = xSemaphoreCreateMutex();
    TickType_t ticks                = xTaskGetTickCount();
    SI.last_frame_timestamp.tv_sec  = ticks / configTICK_RATE_HZ;
    SI.last_frame_timestamp.tv_usec = (ticks % configTICK_RATE_HZ) * 1e6 / configTICK_RATE_HZ;
}

void startRemoteProxy(Proto through = PROTO_UART) {
    switch (through) {
    case PROTO_UART: {
#ifdef ESP32
        static HardwareSerial atSerial(0);
#else
    #define atSerial Serial1
#endif
        atSerial.setRxBufferSize(COM_BUFFER_SIZE);

        // Hardware reset pulse to Himax WE2 via Pin D3 (RST)
        Serial.println("[PROXY] Pulsing D3 (RST) LOW for 200ms to reset Vision AI module...");
        pinMode(D3, OUTPUT);
        digitalWrite(D3, LOW);
        delay(200);
        digitalWrite(D3, HIGH);
        delay(100);
        pinMode(D3, INPUT_PULLUP);

        AI.begin(&atSerial, -1, 921600);
        AI.set_rx_buffer(64 * 1024);
        atSerial.begin(921600, SERIAL_8N1, D7, D6);
        atSerial.setRxBufferSize(COM_BUFFER_SIZE);

        delay(100);
        int avail = atSerial.available();
        Serial.printf("[PROXY] Vision AI UART ready! Initial bytes in buffer: %d\n", avail);
        break;
    }
    case PROTO_I2C: {
        Wire.setBufferSize(COM_BUFFER_SIZE);
        Wire.begin(SDA, SCL);
        bool ok = AI.begin(&Wire);
        Serial.printf("[PROXY] AI.begin(I2C) -> %s\n", ok ? "SUCCESS" : "FAILED");
        break;
    };
    case PROTO_SPI: {
        SPI.begin(SCK, MOSI, MISO, -1);
        bool ok = AI.begin(&SPI, D1, D0, -1, 15000000);
        Serial.printf("[PROXY] AI.begin(SPI) -> %s\n", ok ? "SUCCESS" : "FAILED");
        break;
    };
    default:
        assert(false && "Unknown proto...");
    }
}

inline uint16_t getMsgType(const char* resp, size_t len) {
    uint16_t type = MSG_TYPE_UNKNOWN;

    if (strnstr(resp, MSG_REPLY_STR, len) != NULL) {
        type |= MSG_TYPE_REPLY;
    } else if (strnstr(resp, MSG_EVENT_STR, len) != NULL) {
        type |= MSG_TYPE_EVENT;
    } else if (strnstr(resp, MSG_LOGGI_STR, len) != NULL) {
        type |= MSG_TYPE_LOGGI;
    } else {
        log_w("Unknown message type...");
    }

    return type;
}

inline uint16_t getCmdType(const char* resp, size_t len) {
    uint16_t type = CMD_TYPE_UNKNOWN;

    if (strnstr(resp, CMD_SAMPLE_STR, len) != NULL) {
        type |= CMD_TYPE_SAMPLE;
    } else if (strnstr(resp, CMD_INVOKE_STR, len) != NULL) {
        type |= CMD_TYPE_INVOKE;
    }

    return type;
}

static void proxyCallback(const char* resp, size_t len) {
    static timeval timestamp;
    TickType_t     ticks = xTaskGetTickCount();
    timestamp.tv_sec     = ticks / configTICK_RATE_HZ;
    timestamp.tv_usec    = (ticks % configTICK_RATE_HZ) * 1e6 / configTICK_RATE_HZ;

    if (!len) {
        log_i("Response is empty...");
        return;
    }

    uint16_t type = 0;
    type |= getMsgType(resp, len);
    if (type == MSG_TYPE_UNKNOWN) {
        return;
    }
    type |= getCmdType(resp, len);

    char* copy = (char*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        copy = (char*)malloc(len);
    }
    if (copy == NULL) {
        log_e("Failed to allocate resp copy...");
        return;
    }
    memcpy(copy, resp, len);

    size_t           limit  = PB.limit;
    PtrBuffer::Slot* p_slot = (PtrBuffer::Slot*)malloc(sizeof(PtrBuffer::Slot));
    if (p_slot == NULL) {
        log_e("Failed to allocate slot...");
        return;
    }

    p_slot->id        = PB.id;
    p_slot->type      = type;
    p_slot->data      = copy;
    p_slot->size      = len;
    p_slot->timestamp = timestamp;

    size_t discarded = 0;
    xSemaphoreTake(PB.mutex, portMAX_DELAY);
    while (PB.slots.size() >= limit) {
        PB.slots.pop_front();
        discarded += 1;
    }
    PB.slots.emplace_back(std::shared_ptr<PtrBuffer::Slot>(p_slot, [](PtrBuffer::Slot* p) {
        if (p == NULL) {
            return;
        }
        if (p->data != NULL) {
            free(p->data);
            p->data = NULL;
        }
        free(p);
    }));
    xSemaphoreGive(PB.mutex);
    PB.id += 1;

    if (discarded > 0) {
        log_i("Discarded %u old responses...", discarded);
    }

    static size_t frame_count = 0;
    bool has_image = (strnstr(resp, MSG_IMAGE_KEY, len) != NULL);
    if (has_image) {
        frame_count++;
        g_total_frames = frame_count;
        g_last_frame_bytes = len;
        g_last_frame_millis = millis();
        // Fast broadcast real-time JPEG binary directly to WebSocket clients
        if (webSocket.connectedClients() > 0 || cloudClient.isConnected()) {
            const char* slice = strnstr(resp, MSG_IMAGE_KEY MSG_QUOTE_STR, len);
            if (slice != NULL) {
                size_t offset = (slice - resp) + strlen(MSG_IMAGE_KEY MSG_QUOTE_STR);
                const char* data = resp + offset;
                const char* quote = strnstr(data, MSG_QUOTE_STR, len - offset);
                if (quote != NULL) {
                    size_t img_len = quote - data;
                    if (img_len > 0) {
                        static uint8_t* ws_jpeg_buf = NULL;
                        if (ws_jpeg_buf == NULL) {
                            ws_jpeg_buf = (uint8_t*)heap_caps_malloc(JPG_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (ws_jpeg_buf == NULL) {
                                ws_jpeg_buf = (uint8_t*)malloc(JPG_BUFFER_SIZE);
                            }
                        }
                        if (ws_jpeg_buf != NULL) {
                            size_t ws_jpeg_size = 0;
                            if (mbedtls_base64_decode(
                                  ws_jpeg_buf, JPG_BUFFER_SIZE, &ws_jpeg_size, (const unsigned char*)data, img_len) == 0) {
                                for (size_t i = 0; i + 1 < ws_jpeg_size; ++i) {
                                    if (ws_jpeg_buf[i] == 0xFF && ws_jpeg_buf[i + 1] == 0xD9) {
                                        ws_jpeg_size = i + 2;
                                        break;
                                    }
                                }
                                webSocket.broadcastBIN((const uint8_t*)ws_jpeg_buf, ws_jpeg_size);
                                if (cloudClient.isConnected()) {
                                    cloudClient.sendBIN((const uint8_t*)ws_jpeg_buf, ws_jpeg_size);
                                }
                            }
                        }
                    }
                }
            }

            // Fast broadcast emotion & box metadata to WebSocket clients
            const char* b = strnstr(resp, "\"boxes\":", len);
            bool sent_box = false;
            if (b != NULL) {
                const char* b_open = strchr(b, '[');
                if (b_open != NULL && b_open[1] == '[') {
                    int cx = 0, cy = 0, cw = 0, ch = 0, score = 0, target = -1;
                    if (sscanf(b_open + 2, "%d,%d,%d,%d,%d,%d", &cx, &cy, &cw, &ch, &score, &target) >= 6) {
                        int x = cx - cw / 2;
                        int y = cy - ch / 2;
                        if (x < 0) x = 0;
                        if (y < 0) y = 0;
                        const char* emotion_names[] = {"angry", "focus", "relax", "scared"};
                        const char* emotion = (target >= 0 && target < 4) ? emotion_names[target] : "relax";

                        char raw_boxes[128];
                        const char* b_close = strstr(b_open, "]]");
                        if (b_close != NULL && (size_t)(b_close + 2 - b_open) < sizeof(raw_boxes)) {
                            size_t raw_len = (b_close + 2) - b_open;
                            memcpy(raw_boxes, b_open, raw_len);
                            raw_boxes[raw_len] = '\0';
                        } else {
                            snprintf(raw_boxes, sizeof(raw_boxes), "[[%d,%d,%d,%d,%d,%d]]", cx, cy, cw, ch, score, target);
                        }

                        char ws_buf[320];
                        snprintf(ws_buf, sizeof(ws_buf),
                            "{\"emotion\":\"%s\",\"confidence\":%.2f,\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},\"raw\":%s,\"timestamp\":%lu}",
                            emotion, score / 100.0f, x, y, cw, ch, raw_boxes, (unsigned long)millis()
                        );
                        webSocket.broadcastTXT(ws_buf);
                        if (cloudClient.isConnected()) {
                            cloudClient.sendTXT(ws_buf);
                        }
                        sent_box = true;
                    }
                }
            }
            if (!sent_box) {
                char ws_buf[128];
                snprintf(ws_buf, sizeof(ws_buf),
                    "{\"emotion\":\"none\",\"confidence\":0.0,\"bbox\":null,\"raw\":[],\"timestamp\":%lu}",
                    (unsigned long)millis()
                );
                webSocket.broadcastTXT(ws_buf);
                if (cloudClient.isConnected()) {
                    cloudClient.sendTXT(ws_buf);
                }
            }
        }
    }

    log_i("Received %u bytes...", len);
}

static uint32_t s_last_diag_ms = 0;
void loopRemoteProxy() {
    for (int i = 0; i < 3; i++) {
        webSocket.loop();
    }
    cloudClient.loop();
    AI.fetch(proxyCallback);

    uint32_t now = millis();
    if (now - s_last_diag_ms > 2500) {
        s_last_diag_ms = now;
        uint32_t elapsed = (g_last_frame_millis > 0) ? (now - g_last_frame_millis) : 0;
        char diag[160];
        snprintf(diag, sizeof(diag),
            "{\"type\":\"diag\",\"total_frames\":%lu,\"elapsed_ms\":%lu,\"free_psram\":%lu}",
            (unsigned long)g_total_frames, (unsigned long)elapsed, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
        );
        webSocket.broadcastTXT(diag);
        if (cloudClient.isConnected()) {
            cloudClient.sendTXT(diag);
        }
    }
}

typedef struct {
    httpd_req_t* req;
    size_t       len;
} jpg_chunking_t;

#define PART_BOUNDARY "frame"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t web_httpd    = NULL;
httpd_handle_t stream_httpd = NULL;

typedef struct {
    size_t size;   //number of values used for filtering
    size_t index;  //current value index
    size_t count;  //value count
    int    sum;
    int*   values;  //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t* ra_filter_init(ra_filter_t* filter, size_t sample_size) {
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int*)malloc(sample_size * sizeof(int));
    if (!filter->values) {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t* filter, int value) {
    if (!filter->values) {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}
#endif

static esp_err_t results_handler(httpd_req_t* req) {
    esp_err_t     res     = ESP_OK;
    static size_t last_id = 0;
    static char*  hdr_buf[128];
    static char*  rst_buf = NULL;
    if (rst_buf == NULL) {
        rst_buf = (char*)malloc(RST_BUFFER_SIZE);
        if (rst_buf == NULL) {
            log_e("Failed to allocate results buffer...");
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
    }

    std::shared_ptr<PtrBuffer::Slot> slot = nullptr;

    TickType_t time_begin = xTaskGetTickCount();
    while ((xTaskGetTickCount() - time_begin) < RESULT_TIMEOUT_MS) {
        xSemaphoreTake(PB.mutex, portMAX_DELAY);
        auto slots = PB.slots;
        xSemaphoreGive(PB.mutex);

        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            if (it->get()->id <= last_id) {
                break;
            }
            if (it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_SAMPLE) ||
                it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_INVOKE)) {
                slot    = *it;
                last_id = slot->id;
                break;
            }
        }

        if (!slot) {
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        break;
    }

    if (slot == nullptr) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        const char* empty_res = "{\"type\":1,\"name\":\"INVOKE\",\"code\":0,\"data\":{\"boxes\":[]}}\r\n";
        return httpd_resp_send(req, empty_res, strlen(empty_res));
    }

    const char* img_head = strnstr((const char*)slot->data, MSG_IMAGE_KEY MSG_QUOTE_STR, slot->size);
    if (img_head != NULL) {
        size_t offset = (img_head - (const char*)slot->data) + strlen(MSG_IMAGE_KEY MSG_QUOTE_STR);

        bool        found_prefix_comma = false;
        const char* img_head_full      = img_head - strlen(MSG_COMMA_STR);
        if (img_head_full >= (const char*)slot->data) {
            if (strncmp(img_head_full, MSG_COMMA_STR, strlen(MSG_COMMA_STR)) == 0) {
                img_head           = img_head_full;
                found_prefix_comma = true;
            }
        }

        const char* img_tail = strnstr((const char*)slot->data + offset, MSG_QUOTE_STR, slot->size - offset);
        if (img_tail == NULL) {
            log_e("Broken json format...");
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        offset = (img_tail - (const char*)slot->data) + strlen(MSG_QUOTE_STR);

        if (!found_prefix_comma) {
            const char* img_tail_full = strnstr((const char*)slot->data + offset, MSG_COMMA_STR, slot->size - offset);
            if (img_tail_full != NULL) {
                img_tail = img_tail_full;
            }
        }

        if (slot->size - (img_tail - img_head) >= RST_BUFFER_SIZE) {
            log_e("Results buffer is not enough...");
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        memset(rst_buf, 0, RST_BUFFER_SIZE);
        size_t size   = img_head - (const char*)slot->data;
        size_t copied = 0;
        strncpy(rst_buf, (const char*)slot->data, size);
        copied += size;
        size = ((const char*)slot->data + slot->size) - img_tail;
        strncpy(rst_buf + copied, img_tail, size);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32] = {0};
    snprintf(ts, sizeof(ts), "%ld", slot->id);
    httpd_resp_set_hdr(req, "X-Id", (const char*)ts);

    memset(ts, 0, sizeof(ts));
    snprintf(ts, sizeof(ts), "%ld.%06ld", slot->timestamp.tv_sec, slot->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char*)ts);

    size_t  last_frame_id;
    timeval last_frame_timestamp;

    xSemaphoreTake(SI.mutex, portMAX_DELAY);
    last_frame_id        = SI.last_frame_id;
    last_frame_timestamp = SI.last_frame_timestamp;
    xSemaphoreGive(SI.mutex);

    memset(ts, 0, sizeof(ts));
    snprintf(ts, sizeof(ts), "%ld", last_frame_id);
    httpd_resp_set_hdr(req, "X-Last-Frame-Id", (const char*)ts);

    memset(ts, 0, sizeof(ts));
    snprintf(ts, sizeof(ts), "%ld.%06ld", last_frame_timestamp.tv_sec, last_frame_timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Last-Frame-Timestamp", (const char*)ts);

    res = httpd_resp_send(req, (const char*)rst_buf, strlen(rst_buf));
    if (res != ESP_OK) {
        log_e("Send results failed...");
    }

    return res;
}

static esp_err_t stream_frame_handler(httpd_req_t* req) {
    esp_err_t res = ESP_OK;
    size_t    last_id  = 0;

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    // Enable TCP_NODELAY and socket buffer tuning
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        int sndbuf = 32768;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    }

    // Allocate buffer with 256 bytes headroom for multipart frame boundary header + trailing \r\n
    char* send_buf = (char*)malloc(JPG_BUFFER_SIZE + 256);
    if (send_buf == NULL) {
        log_e("Failed to allocate jpeg send buffer...");
        return ESP_ERR_NO_MEM;
    }
    char* jpeg_buf = send_buf + 128;

    while (true) {
        std::shared_ptr<PtrBuffer::Slot> slot = nullptr;

        {
            xSemaphoreTake(PB.mutex, portMAX_DELAY);
            // Always pick the NEWEST available frame (never lag behind with stale queue)
            for (auto it = PB.slots.rbegin(); it != PB.slots.rend(); ++it) {
                if (it->get()->id > last_id) {
                    if (it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_SAMPLE) ||
                        it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_INVOKE)) {
                        slot    = *it;
                        last_id = slot->id;
                        break;
                    }
                }
            }
            xSemaphoreGive(PB.mutex);

            if (!slot) {
                if (sockfd >= 0) {
                    char dummy;
                    int r = recv(sockfd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        break;
                    }
                }
                vTaskDelay(20 / portTICK_PERIOD_MS);
                continue;
            }
        }

        const char* slice = strnstr((const char*)slot->data, MSG_IMAGE_KEY MSG_QUOTE_STR, slot->size);
        if (slice == NULL) {
            continue;
        }
        size_t      offset = (slice - (const char*)slot->data) + strlen(MSG_IMAGE_KEY MSG_QUOTE_STR);
        const char* data   = (const char*)slot->data + offset;
        const char* quote  = strnstr(data, MSG_QUOTE_STR, slot->size - offset);
        if (quote == NULL) {
            continue;
        }
        size_t len = quote - data;
        if (len == 0) {
            continue;
        }

        size_t jpeg_size = 0;
        if (mbedtls_base64_decode(
              (unsigned char*)jpeg_buf, JPG_BUFFER_SIZE, &jpeg_size, (const unsigned char*)data, len) != 0) {
            log_e("Failed to decode image data...");
            continue;
        }

        // Trim trailing padding bytes after JPEG EOI marker (\xff\xd9)
        for (size_t i = 0; i + 1 < jpeg_size; ++i) {
            if ((uint8_t)jpeg_buf[i] == 0xFF && (uint8_t)jpeg_buf[i + 1] == 0xD9) {
                jpeg_size = i + 2;
                break;
            }
        }

        xSemaphoreTake(SI.mutex, portMAX_DELAY);
        SI.last_frame_id        = slot->id;
        SI.last_frame_timestamp = slot->timestamp;
        xSemaphoreGive(SI.mutex);

        char part_hdr[128];
        int hlen = snprintf(part_hdr, sizeof(part_hdr),
            "--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned int)jpeg_size);

        // Prepend multipart header directly before JPEG bytes
        memcpy(jpeg_buf - hlen, part_hdr, hlen);

        // Append trailing \r\n delimiter after JPEG data
        jpeg_buf[jpeg_size]     = '\r';
        jpeg_buf[jpeg_size + 1] = '\n';

        if (sockfd >= 0) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sockfd, &write_fds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms max wait
            int sel = select(sockfd + 1, NULL, &write_fds, NULL, &tv);
            if (sel <= 0) {
                if (sel < 0) {
                    log_e("Socket error or client closed");
                    break;
                }
                // Send buffer not ready; drop this frame to keep stream real-time
                continue;
            }
        }

        res = httpd_resp_send_chunk(req, jpeg_buf - hlen, hlen + jpeg_size + 2);
        if (res != ESP_OK) {
            log_e("Send frame failed or client disconnected...");
            break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    free(send_buf);
    return res;
}

static esp_err_t stream_result_handler(httpd_req_t* req) {
    esp_err_t     res     = ESP_OK;
    static size_t last_id = 0;

#if BYTE_TRACKER_ENABLED
    JsonDocument                     response;
    BYTETracker                      tracker;
    std::vector<BYTETracker::Object> boxes_list;
    static char*                     rsp_buf = NULL;
    if (rsp_buf == NULL) {
        rsp_buf = (char*)malloc(RSP_BUFFER_SIZE);
        if (rsp_buf == NULL) {
            log_e("Failed to allocate response buffer...");
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
    }
#endif

    res |= httpd_resp_set_status(req, HTTPD_200);
    res |= httpd_resp_set_type(req, "application/json");
    res |= httpd_resp_set_hdr(req, "Connection", "keep-alive");
    res |= httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (res != ESP_OK) {
        log_e("Failed to set response headers...");
        return res;
    }

    while (res == ESP_OK) {
        std::shared_ptr<PtrBuffer::Slot> slot = nullptr;

        xSemaphoreTake(PB.mutex, portMAX_DELAY);
        auto slots = PB.slots;
        xSemaphoreGive(PB.mutex);

        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            if (it->get()->id <= last_id) {
                break;
            }
            if (it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_SAMPLE) ||
                it->get()->type == (MSG_TYPE_EVENT | CMD_TYPE_INVOKE)) {
                slot    = *it;
                last_id = slot->id;
                break;
            }
        }

        if (!slot) {
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        switch (slot->type) {
        case MSG_TYPE_EVENT | CMD_TYPE_SAMPLE: {
            res |= httpd_resp_send_chunk(req, (const char*)slot->data, slot->size);
            res |= httpd_resp_send_chunk(req, MSG_TERMI_STR, strlen(MSG_TERMI_STR));
            break;
        }

        case MSG_TYPE_EVENT | CMD_TYPE_INVOKE: {
#if !BYTE_TRACKER_ENABLED
            res |= httpd_resp_send_chunk(req, (const char*)slot->data, slot->size);
            res |= httpd_resp_send_chunk(req, MSG_TERMI_STR, strlen(MSG_TERMI_STR));
            break;
#else
            response.clear();
            DeserializationError err = deserializeJson(response, (const char*)slot->data, slot->size);
            if (err != DeserializationError::Ok) {
                log_e("Failed to parse json...");
                log_e("%s\n", (const char*)slot->data);
                break;
            }

            if (!response.containsKey("data")) {
                log_e("No data found in json...");
                break;
            }

            boxes_list.clear();
            if (response["data"].containsKey("boxes")) {
                JsonArray boxes = response["data"]["boxes"];
                for (JsonArray box : boxes) {
                    if (box.size() != 6) {
                        log_w("Invalid box size...");
                        continue;
                    }
                    BYTETracker::Object cxcywh;
                    cxcywh.rect.x      = box[0];
                    cxcywh.rect.y      = box[1];
                    cxcywh.rect.width  = box[2];
                    cxcywh.rect.height = box[3];
                    cxcywh.label       = box[5];
                    cxcywh.prob        = box[4];
                    boxes_list.push_back(cxcywh);
                }

                std::vector<STrack> output_stracks = tracker.update(boxes_list);

                boxes.clear();
                for (STrack& strack : output_stracks) {
                    JsonDocument doc;
                    JsonArray    box = doc.to<JsonArray>();
                    box.add(static_cast<int32_t>(strack.tlwh[0]));
                    box.add(static_cast<int32_t>(strack.tlwh[1]));
                    box.add(static_cast<int32_t>(strack.tlwh[2]));
                    box.add(static_cast<int32_t>(strack.tlwh[3]));
                    box.add(static_cast<int32_t>(strack.score));
                    box.add(static_cast<int32_t>(strack.label));
                    box.add(static_cast<int32_t>(strack.track_id));
                    boxes.add(box);
                }

            } else if (response["data"].containsKey("keypoints")) {
                JsonArray keypoints = response["data"]["keypoints"];

                size_t id = 0;
                for (JsonArray keypoint : keypoints) {
                    if (keypoint.size() != 2) {
                        log_w("Invalid keypoint size...");
                        continue;
                    }
                    JsonArray box = keypoint[0];
                    if (box.size() != 6) {
                        log_w("Invalid box size...");
                        continue;
                    }
                    BYTETracker::Object cxcywh;
                    cxcywh.rect.x      = box[0];
                    cxcywh.rect.y      = box[1];
                    cxcywh.rect.width  = box[2];
                    cxcywh.rect.height = box[3];
                    cxcywh.prob        = box[4];
                    cxcywh.label       = box[5];
                    cxcywh.label       = id++ << 16 | (cxcywh.label & 0xffff);
                    box[5]             = cxcywh.label;

                    boxes_list.push_back(cxcywh);
                }

                std::vector<STrack> output_stracks = tracker.update(boxes_list);

                for (JsonArray keypoint : keypoints) {
                    JsonArray box = keypoint[0];
                    if (box.size() != 6) {
                        log_w("Invalid box size...");
                        continue;
                    }

                    int  label = box[5];
                    auto it = std::find_if(output_stracks.begin(), output_stracks.end(), [label](const STrack& strack) {
                        return strack.label == label;
                    });
                    if (it != output_stracks.end()) {
                        box[0] = static_cast<int32_t>(it->tlwh[0]);
                        box[1] = static_cast<int32_t>(it->tlwh[1]);
                        box[2] = static_cast<int32_t>(it->tlwh[2]);
                        box[3] = static_cast<int32_t>(it->tlwh[3]);
                        box[4] = static_cast<int32_t>(it->score);
                        box[5] = static_cast<int32_t>(it->label & 0xffff);
                        box.add(static_cast<int32_t>(it->track_id));

                        output_stracks.erase(it);
                    } else {
                        box[5] = static_cast<int32_t>(label & 0xffff);
                        box.add(0);
                    }
                }
            }

            size_t len = serializeJson(response, rsp_buf, RSP_BUFFER_SIZE - sizeof(MSG_TERMI_STR));
            for (size_t i = 0; i < strlen(MSG_TERMI_STR); ++i, ++len) {
                rsp_buf[len] = MSG_TERMI_STR[i];
            }
            rsp_buf[len] = '\0';
            res          = httpd_resp_send_chunk(req, rsp_buf, len);

            break;
#endif
        }

        default:;
        }

        if (res != ESP_OK) {
            log_e("Send results failed...");
            break;
        }
    }

    return res;
}

static esp_err_t parse_get(httpd_req_t* req, char** obuf) {
    char*  buf     = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t command_handler(httpd_req_t* req) {
    char* buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        log_e("Failed to parse get data...");
        return ESP_FAIL;
    }

    char* qry_buf = (char*)malloc(QRY_BUFFER_SIZE);
    if (qry_buf == NULL) {
        free(buf);
        log_e("Failed to allocate query buffer...");
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    memset(qry_buf, 0, QRY_BUFFER_SIZE);
    if (httpd_query_key_value(buf, "base64", qry_buf, QRY_BUFFER_SIZE - 1) != ESP_OK) {
        free(buf);
        free(qry_buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    char* cmd_buf = (char*)malloc(CMD_BUFFER_SIZE);
    if (cmd_buf == NULL) {
        free(qry_buf);
        log_e("Failed to allocate cmd buffer...");
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    size_t cmd_size = 0;
    memset(cmd_buf, 0, CMD_BUFFER_SIZE);
    if (mbedtls_base64_decode(
          (unsigned char*)cmd_buf, CMD_BUFFER_SIZE, &cmd_size, (const unsigned char*)qry_buf, strlen(qry_buf)) != 0) {
        free(qry_buf);
        free(cmd_buf);
        log_e("Failed to decode cmd data...");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    free(qry_buf);

    TickType_t ticks           = xTaskGetTickCount();
    char       cmd_tag_buf[32] = {0};
    size_t     cmd_tag_size    = snprintf(cmd_tag_buf, sizeof(cmd_tag_buf), CMD_TAG_FMT_STR, ticks);

    size_t last_id = PB.id;

    Serial.printf("[HTTP->AI] CMD: %.*s (tag: %s)\n", (int)cmd_size, cmd_buf, cmd_tag_buf);

    // Intercept frontend commands to prevent web UI crashes and provide cat emotion classes
    if (strstr(cmd_buf, "SENSOR?") != NULL) {
        Serial.printf("[HTTP] SENSOR? -> opt_id: 0\n");
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{\"opt_id\":0}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "SENSOR") != NULL) {
        Serial.printf("[HTTP] SENSOR set -> OK\n");
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "ACTION?") != NULL) {
        Serial.printf("[HTTP] ACTION? -> action: 0\n");
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{\"action\":0}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "ACTION") != NULL) {
        Serial.printf("[HTTP] ACTION set -> OK\n");
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "INFO") != NULL) {
        Serial.printf("[HTTP] INFO? -> cat emotion classes\n");
        // Base64 of {"classes":["angry","focus","relax","scared"]} is eyJjbGFzc2VzIjpbImFuZ3J5IiwiZm9jdXMiLCJyZWxheCIsInNjYXJlZCJdfQ==
        char reply[256];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{\"crc16_maxim\":0,\"info\":\"eyJjbGFzc2VzIjpbImFuZ3J5IiwiZm9jdXMiLCJyZWxheCIsInNjYXJlZCJdfQ==\"}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "INVOKE") != NULL) {
        Serial.printf("[HTTP] INVOKE -> algorithm config OK\n");
        char reply[256];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{\"algorithm\":{\"config\":{\"tscore\":25,\"tiou\":45}}}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }
    if (strstr(cmd_buf, "TSCORE") != NULL || strstr(cmd_buf, "TIOU") != NULL || strstr(cmd_buf, "BREAK") != NULL || strstr(cmd_buf, "SAMPLE") != NULL) {
        Serial.printf("[HTTP] Handling %s -> OK\n", cmd_buf);
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s%s\",\"code\":0,\"data\":{}}\r\n", cmd_tag_buf, cmd_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        free(cmd_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }

    AI.write(CMD_PREFIX, strlen(CMD_PREFIX));
    AI.write(cmd_tag_buf, cmd_tag_size);
    AI.write(cmd_buf, cmd_size);
    free(cmd_buf);
    AI.write(CMD_SUFFIX, strlen(CMD_SUFFIX));

    std::shared_ptr<PtrBuffer::Slot> slot = nullptr;

    TickType_t time_begin = xTaskGetTickCount();
    while ((xTaskGetTickCount() - time_begin) < (RESULT_TIMEOUT_MS / portTICK_PERIOD_MS)) {
        vTaskDelay(5 / portTICK_PERIOD_MS);

        xSemaphoreTake(PB.mutex, portMAX_DELAY);
        auto slots = PB.slots;
        xSemaphoreGive(PB.mutex);

        auto it = std::find_if(slots.begin(), slots.end(), [&](std::shared_ptr<PtrBuffer::Slot> p) {
            if (p->id - last_id <= 0) {
                return false;
            }

            if (p->type & MSG_TYPE_REPLY || p->type & MSG_TYPE_LOGGI) {
                const char* tag = strnstr((const char*)p->data, cmd_tag_buf, p->size);
                if (tag != NULL) {
                    return true;
                }
            }

            last_id = p->id;
            return false;
        });
        if (it == slots.end()) {
            continue;
        }

        slot = *it;
        break;
    }

    if (slot == nullptr) {
        Serial.printf("[HTTP->AI] TIMEOUT waiting for reply to tag %s -> returning JSON error fallback\n", cmd_tag_buf);
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"type\":0,\"name\":\"%s\",\"code\":-1,\"error\":\"timeout\"}\r\n", cmd_tag_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);
        return httpd_resp_send(req, reply, strlen(reply));
    }

    Serial.printf("[HTTP->AI] Reply received: %.*s\n", (int)min((size_t)120, slot->size), (const char*)slot->data);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Cmd-Tag", cmd_tag_buf);

    return httpd_resp_send(req, (const char*)slot->data, slot->size);
}

static esp_err_t index_handler(httpd_req_t* req) {
    Serial.printf("[HTTP] GET / from client %d\n", httpd_req_to_sockfd(req));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char*)web_index_html_gz, web_index_html_gz_len);
}

static char s_app_session_pin[16] = "------";

void setSessionPin(const char* pin) {
    if (pin && strlen(pin) > 0) {
        strncpy(s_app_session_pin, pin, sizeof(s_app_session_pin) - 1);
        s_app_session_pin[sizeof(s_app_session_pin) - 1] = '\0';
    }
}

const char* getSessionPin() {
    return s_app_session_pin;
}

static esp_err_t pin_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"pin\":\"%s\"}\n", s_app_session_pin);
    return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t status_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[1280];
    uint32_t now = millis();
    uint32_t elapsed = (g_last_frame_millis > 0) ? (now - g_last_frame_millis) : 0;
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<meta http-equiv='refresh' content='3'>"
        "<title>Cat Emotion Status</title>"
        "<style>body{font-family:sans-serif;padding:20px;max-width:600px;margin:auto;background:#f5f5f7;}"
        ".card{background:white;padding:20px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,0.1);margin-bottom:16px;}"
        ".pin-box{background:#eef7ff;border:2px solid #0071e3;border-radius:12px;padding:16px;margin-bottom:16px;text-align:center;}"
        ".pin-num{font-size:36px;font-weight:bold;letter-spacing:6px;color:#0071e3;margin:8px 0;font-family:monospace;}"
        "h2{margin-top:0;color:#333;}.ok{color:#2ecc71;font-weight:bold;}.warn{color:#e67e22;font-weight:bold;}"
        "a{display:inline-block;margin-top:8px;color:#0071e3;text-decoration:none;font-weight:bold;}"
        "</style></head><body>"
        "<div class='pin-box'>"
        "<h3 style='margin:0;color:#0071e3;'>Current Session PIN</h3>"
        "<div class='pin-num'>%s</div>"
        "<p style='color:#555;font-size:13px;margin:0;'>Enter this PIN on the web dashboard to unlock the live stream.</p>"
        "</div>"
        "<div class='card'>"
        "<h2>Cat Emotion AI Server</h2>"
        "<p><b>Status:</b> %s</p>"
        "<p><b>Frames Received:</b> %u</p>"
        "<p><b>Last Frame Size:</b> %u bytes</p>"
        "<p><b>Last Frame Received:</b> %u ms ago</p>"
        "<p><b>Free PSRAM:</b> %u bytes</p>"
        "<p><b>Free Heap:</b> %u bytes</p>"
        "</div>"
        "<div class='card'>"
        "<h3>Links</h3>"
        "<p><a href='/'>&gt;&gt; Open Web UI (Boxes & Stream)</a></p>"
        "<p><a href='/pin'>&gt;&gt; JSON PIN Endpoint (/pin)</a></p>"
        "<p><a href='http://192.168.4.1:8080/stream'>&gt;&gt; Direct MJPEG Stream (Port 8080)</a></p>"
        "</div>"
        "</body></html>",
        s_app_session_pin,
        (g_total_frames > 0 && elapsed < 4000) ? "<span class='ok'>Streaming Active</span>" : (g_total_frames > 0 ? "<span class='warn'>Idle</span>" : "<span class='warn'>Waiting for frames</span>"),
        (unsigned int)g_total_frames,
        (unsigned int)g_last_frame_bytes,
        (unsigned int)elapsed,
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned int)esp_get_free_heap_size()
    );
    return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t stream_redirect_handler(httpd_req_t* req) {
    char host_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, sizeof(host_hdr)) != ESP_OK || strlen(host_hdr) == 0) {
        strncpy(host_hdr, "192.168.68.100", sizeof(host_hdr) - 1);
    } else {
        char* colon = strchr(host_hdr, ':');
        if (colon) *colon = '\0';
    }
    char redirect_url[96];
    snprintf(redirect_url, sizeof(redirect_url), "http://%s:8080/stream", host_hdr);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

void startCameraServer() {
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.ctrl_port        = ESP_HTTPD_DEF_CTRL_PORT;
    config.max_uri_handlers = 20;
    config.stack_size       = 12288;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 2;
    config.recv_wait_timeout = 2;
    config.core_id          = 0;
    config.task_priority    = 1;

    httpd_uri_t index_uri = {.uri      = "/",
                             .method   = HTTP_GET,
                             .handler  = index_handler,
                             .user_ctx = NULL};

    httpd_uri_t status_uri = {.uri      = "/status",
                              .method   = HTTP_GET,
                              .handler  = status_handler,
                              .user_ctx = NULL};

    httpd_uri_t command_uri = {.uri      = "/command",
                               .method   = HTTP_GET,
                               .handler  = command_handler,
                               .user_ctx = NULL};

    httpd_uri_t result_uri = {.uri      = "/result",
                              .method   = HTTP_GET,
                              .handler  = results_handler,
                              .user_ctx = NULL};

    httpd_uri_t stream_frame_uri = {.uri      = "/stream/frame",
                                    .method   = HTTP_GET,
                                    .handler  = stream_frame_handler,
                                    .user_ctx = NULL};

    httpd_uri_t stream_result_uri = {.uri      = "/stream/result",
                                     .method   = HTTP_GET,
                                     .handler  = stream_result_handler,
                                     .user_ctx = NULL};

    ra_filter_init(&ra_filter, 20);

    esp_err_t ret = ESP_OK;

    httpd_uri_t stream_redirect_uri = {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = stream_redirect_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = stream_frame_handler,
        .user_ctx = NULL
    };

    httpd_uri_t pin_uri = {.uri      = "/pin",
                           .method   = HTTP_GET,
                           .handler  = pin_handler,
                           .user_ctx = NULL};

    Serial.printf("[HTTP] Starting web server on port: %d ...\n", config.server_port);
    if ((ret = httpd_start(&web_httpd, &config)) == ESP_OK) {
        httpd_register_uri_handler(web_httpd, &index_uri);
        httpd_register_uri_handler(web_httpd, &status_uri);
        httpd_register_uri_handler(web_httpd, &pin_uri);
        httpd_register_uri_handler(web_httpd, &result_uri);
        httpd_register_uri_handler(web_httpd, &command_uri);
        httpd_register_uri_handler(web_httpd, &stream_redirect_uri);
        Serial.println("[HTTP] Web server (port 80) started successfully!");
    } else {
        Serial.printf("[HTTP] ERROR: Failed to start web server on port 80, code: 0x%x\n", ret);
    }

    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port      = 8080;
    stream_config.ctrl_port        = ESP_HTTPD_DEF_CTRL_PORT + 1;
    stream_config.max_uri_handlers = 8;
    stream_config.stack_size       = 12288;
    stream_config.max_open_sockets = 5;
    stream_config.lru_purge_enable = true;
    stream_config.send_wait_timeout = 2;
    stream_config.recv_wait_timeout = 2;
    stream_config.core_id          = 0;
    stream_config.task_priority    = 1;

    Serial.printf("[HTTP] Starting stream server on port: %d ...\n", stream_config.server_port);
    if ((ret = httpd_start(&stream_httpd, &stream_config)) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_frame_uri);
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &stream_result_uri);
        Serial.println("[HTTP] Stream server (port 8080) started successfully!");
    } else {
        Serial.printf("[HTTP] ERROR: Failed to start stream server on port 8080, code: 0x%x\n", ret);
    }

    webSocket.begin();
    webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
        if (type == WStype_CONNECTED) {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[WebSocket] Client [%u] connected from %s\n", num, ip.toString().c_str());
        } else if (type == WStype_DISCONNECTED) {
            Serial.printf("[WebSocket] Client [%u] disconnected\n", num);
        }
    });
    Serial.println("[WebSocket] Server started on port 81");
}
