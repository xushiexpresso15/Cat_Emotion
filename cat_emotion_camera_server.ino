#include <Seeed_Arduino_SSCMA.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "app_httpd.h"

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

static String s_full_cloud_path;
void connectToCloudRelay() {
    if (!cloud_relay_enabled) return;
    static bool s_cloud_started = false;
    if (s_cloud_started) return;
    s_cloud_started = true;

    s_full_cloud_path = String(cloud_relay_path);
    if (cloud_relay_token && strlen(cloud_relay_token) > 0) {
        s_full_cloud_path += "?token=";
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
    Serial.println();
    Serial.println("=========================================");
    Serial.println("  Cat Emotion Monitoring - Camera Server");
    Serial.println("=========================================");

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

    // 2. 嘗試依序連線已知外部 Wi-Fi (手機熱點優先)
    bool sta_connected = false;
    for (size_t i = 0; i < NUM_KNOWN_NETWORKS; i++) {
        Serial.printf("[WiFi] 正在連線至: %s ...\n", known_networks[i].ssid);
        WiFi.begin(known_networks[i].ssid, known_networks[i].password);
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 16) {
            delay(500);
            Serial.print(".");
            retries++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            sta_connected = true;
            Serial.printf("\n[WiFi] 連線成功！已連線至: %s\n", known_networks[i].ssid);
            Serial.printf("[WiFi] 取得 IP 位址: %s\n", WiFi.localIP().toString().c_str());
            if (MDNS.begin("cat")) {
                Serial.println("[MDNS] 網域名稱已啟用: http://cat.local");
            }
            break;
        } else {
            Serial.printf("\n[WiFi] 未連上 %s\n", known_networks[i].ssid);
            WiFi.disconnect();
            delay(200);
        }
    }

    if (!sta_connected) {
        Serial.println("[WiFi] 外部 Wi-Fi 未連線，維持獨立 AP 熱點運作 (Cat_Emotion_AP: 192.168.4.1)。");
    }

    Serial.println("\n初始化 Vision AI Module V2 (UART 921600 baud)...");
    startRemoteProxy(PROTO_UART);
    startCameraServer();

    if (cloud_relay_enabled && sta_connected) {
        connectToCloudRelay();
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
    Serial.printf("  原生 MJPEG 串流: /stream\n");
    Serial.printf("  即時推論 WebSocket: ws://<IP>:81/\n");
    Serial.println("=========================================\n");
}

static uint32_t s_last_wifi_check_ms = 0;

void loop() {
    dnsServer.processNextRequest();
    loopRemoteProxy();

    // 背景非同步 Wi-Fi 斷線重連檢查（每 15 秒檢查一次，不阻塞串流）
    uint32_t now = millis();
    if (now - s_last_wifi_check_ms > 15000) {
        s_last_wifi_check_ms = now;
        if (WiFi.status() != WL_CONNECTED) {
            // 嘗試非同步重新連線優先網路（手機熱點）
            WiFi.begin(known_networks[0].ssid, known_networks[0].password);
        }
    }
    yield();
}
