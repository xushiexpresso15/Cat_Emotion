#pragma once

// =========================================================================
// Cat Emotion Monitor - Credentials & Secrets Template
// =========================================================================
// Instructions:
// 1. Copy this file to "secrets.h" in the same directory:
//    cp secrets.example.h secrets.h
// 2. Edit "secrets.h" and enter your real Wi-Fi credentials and token.
// 3. "secrets.h" is ignored by git and will NEVER be committed to GitHub.
// =========================================================================

struct KnownNetwork {
    const char* ssid;
    const char* password;
};

// List of known Wi-Fi networks (tried in order)
const KnownNetwork known_networks[] = {
    {"YOUR_HOTSPOT_SSID", "YOUR_HOTSPOT_PASSWORD"},  // Primary (Mobile hotspot / field test)
    {"TANetRoaming",        ""},                     // Campus Wi-Fi (Open / MAC authenticated)
    {"YOUR_WIFI_SSID",    "YOUR_WIFI_PASSWORD"}      // Secondary (Home / lab router)
};
const size_t NUM_KNOWN_NETWORKS = sizeof(known_networks) / sizeof(known_networks[0]);

// Standalone SoftAP Configuration (http://192.168.4.1)
const char* ap_ssid     = "Cat_Emotion_AP";
const char* ap_password = "password123";

// Cloud Relay Configuration
// Set cloud_relay_enabled to true when streaming to a public relay (e.g. Render)
const bool     cloud_relay_enabled = false;
const char*    cloud_relay_host    = "your-relay-service.onrender.com";
const uint16_t cloud_relay_port    = 443;
const char*    cloud_relay_path    = "/esp32";
const bool     cloud_relay_ssl     = true;

// Pre-shared authentication secret for the cloud relay
// Must match STREAM_SECRET on the cloud relay server
const char*    cloud_relay_token   = "your_stream_secret_here";

// Discord Webhook Configuration (for boot PIN and stress alerts)
// Set discord_enabled to true and paste your Discord channel webhook URL.
const bool     discord_enabled     = false;
const char*    discord_webhook_url = "https://discord.com/api/webhooks/YOUR_WEBHOOK_ID/YOUR_WEBHOOK_TOKEN";
