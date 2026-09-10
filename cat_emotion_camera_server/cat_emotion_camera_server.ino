#include <Seeed_Arduino_SSCMA.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "app_httpd.h"
#include "stress_detector.h"
#include "discord_alert.h"

// ===========================
// Configuration & Credentials
// ===========================
// Copy secrets.example.h to secrets.h and fill in your real Wi-Fi credentials and token.
// secrets.h is gitignored and will never be committed to GitHub.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

DNSServer dnsServer;

void initSharedBuffer();
void initStatInfo();

void startRemoteProxy(Proto);
void startCameraServer();

void loopRemoteProxy();

static char s_session_pin[7] = "000000";
static String s_full_cloud_path;
static bool s_boot_alert_sent = false;

void notifyBootToDiscord() {
    if (s_boot_alert_sent) return;
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) return;
    s_boot_alert_sent = true;
    sendDiscordBootNotification(s_session_pin, WiFi.localIP().toString().c_str(), "https://cat-emo-live.onrender.com");
}

void connectToCloudRelay() {
    if (!cloud_relay_enabled) return;
    static bool s_cloud_started = false;
    if (s_cloud_started) return;
    s_cloud_started = true;

    s_full_cloud_path = String(cloud_relay_path) + "?pin=" + String(s_session_pin);
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        s_full_cloud_path += "&ip=";
        s_full_cloud_path += WiFi.localIP().toString();
    }
    if (cloud_relay_token && strlen(cloud_relay_token) > 0) {
        s_full_cloud_path += "&token=";
        s_full_cloud_path += cloud_relay_token;
    }
    initCloudRelay(cloud_relay_host, cloud_relay_port, s_full_cloud_path.c_str(), cloud_relay_ssl);
}

void setup() {
    initSharedBuffer();
    initStatInfo();

    Serial.begin(115200);
    Serial.setTxTimeoutMs(0); // Make USB CDC non-blocking so it never stalls loopTask
    Serial.setDebugOutput(false);

    // Generate random 6-digit session PIN (100000 - 999999) on each boot
    uint32_t rand_val = (esp_random() % 900000) + 100000;
    snprintf(s_session_pin, sizeof(s_session_pin), "%06u", rand_val);
    setSessionPin(s_session_pin);

    Serial.println();
    Serial.println("=========================================");
    Serial.println("  Cat Emotion Monitoring - Camera Server");
    Serial.printf("  [SECURITY] 當次動態觀看 PIN: %s\n", s_session_pin);
    Serial.println("=========================================");

    initStressDetector();
    initDiscordAlert(discord_webhook_url, discord_enabled);

    // 監聽 WiFi 連線與 Station 事件
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
            Serial.printf("[WiFi AP] 客戶端已連線! MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
                info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
        } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            Serial.println("[WiFi AP] 客戶端斷開連線");
        } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
            Serial.printf("[WiFi AP] 分配 IP: %s\n",
                IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            Serial.printf("[WiFi STA] 成功取得 IP: %s\n", WiFi.localIP().toString().c_str());
            MDNS.begin("cat");
            connectToCloudRelay();
            notifyBootToDiscord();
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.println("[WiFi STA] 外部 Wi-Fi 已中斷連線");
        }
    });

    // 啟用 AP + STA 雙模式：熱點恆開，同時連上熱點或路由器 Wi-Fi
    WiFi.mode(WIFI_AP_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setSleep(false);

    // 1. 恆常啟動 SoftAP (192.168.4.1)
    IPAddress apIP(192, 168, 4, 1);
    IPAddress netmask(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, netmask);
    bool ap_ok = WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
    Serial.printf("[WiFi] SoftAP '%s' 狀態: %s (AP IP: %s)\n",
        ap_ssid, ap_ok ? "啟動成功" : "失敗", WiFi.softAPIP().toString().c_str());
    dnsServer.start(53, "*", apIP);

    // 2. 開機 5 秒快速 Wi-Fi 決策：以 1.5 秒快速掃描在場訊號，依優先級選定連線
    bool sta_connected = false;
    Serial.println("[WiFi] 正在快速掃描環境 Wi-Fi (5秒內選定)...");
    int16_t num_scanned = WiFi.scanNetworks(false, false, false, 120);

    int chosen_idx = -1;
    if (num_scanned > 0) {
        // 依優先順序 (1: 手機熱點 -> 2: DECO -> 3: 學校 TANet) 比對在場訊號
        for (size_t p = 0; p < NUM_KNOWN_NETWORKS; p++) {
            for (int i = 0; i < num_scanned; i++) {
                if (WiFi.SSID(i) == known_networks[p].ssid) {
                    chosen_idx = (int)p;
                    break;
                }
            }
            if (chosen_idx >= 0) break;
        }
    }
    WiFi.scanDelete();

    // 選定要連線的目標（若未掃到已知熱點，預設嘗試第 1 順位）
    size_t target_idx = (chosen_idx >= 0) ? (size_t)chosen_idx : 0;
    const KnownNetwork& target_net = known_networks[target_idx];
    Serial.printf("[WiFi] 快速選定連線至: [%s] (優先級第 %d 順位)\n", target_net.ssid, (int)target_idx + 1);

    if (target_net.password && strlen(target_net.password) > 0) {
        WiFi.begin(target_net.ssid, target_net.password);
    } else {
        WiFi.begin(target_net.ssid);
    }

    // 等待連線完成 (最多等待 3.5 秒，確保整體在 5 秒內完成)
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 7) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        sta_connected = true;
        Serial.printf("\n[WiFi] 連線成功！已連線至: %s\n", target_net.ssid);
        Serial.printf("[WiFi] 取得 IP 位址: %s\n", WiFi.localIP().toString().c_str());
        if (MDNS.begin("cat")) {
            Serial.println("[MDNS] 網域名稱已啟用: http://cat.local");
        }
    } else {
        Serial.printf("\n[WiFi] 暫未連上 %s，維持獨立 AP 熱點並於背景重試。\n", target_net.ssid);
    }

    if (!sta_connected) {
        Serial.println("[WiFi] 外部 Wi-Fi 未連線，維持獨立 AP 熱點運作 (Cat_Emotion_AP: 192.168.4.1)。");
    }

    Serial.println("\n初始化 Vision AI Module V2 (UART 921600 baud)...");
    startRemoteProxy(PROTO_UART);
    startCameraServer();

    if (sta_connected) {
        if (cloud_relay_enabled) {
            connectToCloudRelay();
        }
        notifyBootToDiscord();
    }

    Serial.println("\n=========================================");
    Serial.println("  Cat Emotion Camera Server 啟動就緒！");
    if (sta_connected) {
        Serial.printf("  已連線外部網路: %s\n", WiFi.SSID().c_str());
        Serial.printf("  外部網路 IP 訪問: http://%s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  mDNS 網址訪問:   http://cat.local\n");
    }
    if (cloud_relay_enabled) {
        Serial.printf("  雲端公開觀看網址: https://%s\n", cloud_relay_host);
    }
    Serial.printf("  獨立熱點名稱:   %s (密碼: %s)\n", ap_ssid, ap_password);
    Serial.printf("  熱點直連網址:   http://%s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("  熱點查閱 PIN:   http://%s/status\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("  原生 MJPEG 串流: /stream\n");
    Serial.printf("  即時推論 WebSocket: ws://<IP>:81/\n");
    Serial.printf("  當次動態觀看 PIN: %s (雲端網頁解鎖用)\n", s_session_pin);
    Serial.println("=========================================\n");
}

static uint32_t s_last_wifi_check_ms = 0;

void loop() {
    dnsServer.processNextRequest();
    loopRemoteProxy();

    // 背景 Wi-Fi 維持檢查（每 15 秒檢查一次，不阻塞串流）
    uint32_t now = millis();
    if (now - s_last_wifi_check_ms > 15000) {
        s_last_wifi_check_ms = now;
        if (WiFi.status() == WL_CONNECTED) {
            notifyBootToDiscord();
            // 一旦選定並連上任何網路，即持續穩定運作，不做任何背景中斷或切換
        } else {
            // 僅在意外斷線時自動重新連線
            WiFi.reconnect();
        }
    }
    yield();
}
