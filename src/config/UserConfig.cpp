#include "UserConfig.h"
#include "config/BoardConfig.h"
#include <Preferences.h>

// NVS namespace and key for full config backup
static constexpr const char* NVS_NS   = "ratcom_cfg";
static constexpr const char* NVS_KEY  = "json";

// ---------------------------------------------------------------------------
// NVS helpers — bulletproof config storage (internal flash, no SPI bus)
// ---------------------------------------------------------------------------

static bool saveToNVS(const String& json) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = prefs.putString(NVS_KEY, json) > 0;
    prefs.end();
    if (ok) Serial.println("[CONFIG] Saved to NVS");
    return ok;
}

static String loadFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return "";
    String json = prefs.getString(NVS_KEY, "");
    prefs.end();
    return json;
}

// ---------------------------------------------------------------------------
// JSON serialization helpers
// ---------------------------------------------------------------------------

bool UserConfig::parseJson(const String& json) {
    Serial.printf("[CONFIG] Parsing config (%d bytes)\n", json.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
        return false;
    }

    _settings.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
    _settings.loraSF        = doc["lora_sf"]   | (int)LORA_DEFAULT_SF;
    _settings.loraBW        = doc["lora_bw"]   | (long)LORA_DEFAULT_BW;
    _settings.loraCR        = doc["lora_cr"]   | (int)LORA_DEFAULT_CR;
    _settings.loraTxPower   = doc["lora_txp"]  | (int)LORA_DEFAULT_TX_POWER;
    _settings.radioRegion   = doc["radio_region"] | (int)REGION_AMERICAS;

    // WiFi mode — migrate from legacy wifi_enabled bool
    int mode = doc["wifi_mode"] | -1;
    if (mode >= 0) {
        _settings.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
    } else {
        _settings.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
    }
    _settings.wifiAPSSID     = doc["wifi_ap_ssid"]     | "";
    _settings.wifiAPPassword = doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD;
    _settings.wifiSTASSID    = doc["wifi_sta_ssid"]    | "";
    _settings.wifiSTAPassword = doc["wifi_sta_pass"]   | "";

    // TCP outbound connections
    _settings.tcpConnections.clear();
    JsonArray tcpArr = doc["tcp_connections"];
    if (tcpArr) {
        for (JsonObject obj : tcpArr) {
            if (_settings.tcpConnections.size() >= MAX_TCP_CONNECTIONS) break;
            TCPEndpoint ep;
            ep.host = obj["host"] | "";
            ep.port = obj["port"] | TCP_DEFAULT_PORT;
            ep.autoConnect = obj["auto"] | true;
            if (!ep.host.isEmpty()) _settings.tcpConnections.push_back(ep);
        }
    }

    _settings.screenDimTimeout = doc["screen_dim"] | 30;
    _settings.screenOffTimeout = doc["screen_off"] | 60;
    _settings.brightness       = doc["brightness"] | 255;

    _settings.audioEnabled = doc["audio_on"]  | true;
    _settings.audioVolume  = doc["audio_vol"] | 80;

    _settings.utcOffset   = doc["utc_offset"]    | -5;
    _settings.timezoneIdx = doc["tz_idx"]       | 6;
    _settings.timezoneSet = doc["tz_set"]       | false;
    _settings.gpsTimeEnabled = doc["gps_time"] | true;
    _settings.gpsLocationEnabled = doc["gps_loc"] | false;
    _settings.displayName = doc["display_name"] | "";

    // One-time migration: fix old 914.875 MHz default → 915.000 MHz (Reticulum standard).
    // Devices that booted before v1.7.9 persisted 914875000, which is 125 kHz off from
    // the standard Reticulum frequency and prevents interop with RNodes.
    if (_settings.loraFrequency == 914875000) {
        _settings.loraFrequency = 915000000;
        Serial.println("[CONFIG] Migrated frequency 914875000 → 915000000 (Reticulum standard)");
        _migrationDirty = true;
    }

    Serial.printf("[CONFIG] Loaded: wifi_mode=%d ssid='%s' name='%s'\n",
                  (int)_settings.wifiMode,
                  _settings.wifiSTASSID.c_str(),
                  _settings.displayName.c_str());
    return true;
}

String UserConfig::serializeToJson() const {
    JsonDocument doc;

    doc["lora_freq"] = _settings.loraFrequency;
    doc["lora_sf"]   = _settings.loraSF;
    doc["lora_bw"]   = _settings.loraBW;
    doc["lora_cr"]   = _settings.loraCR;
    doc["lora_txp"]  = _settings.loraTxPower;
    doc["radio_region"] = _settings.radioRegion;

    doc["wifi_mode"] = (int)_settings.wifiMode;
    doc["wifi_ap_ssid"] = _settings.wifiAPSSID;
    doc["wifi_ap_pass"] = _settings.wifiAPPassword;
    doc["wifi_sta_ssid"] = _settings.wifiSTASSID;
    doc["wifi_sta_pass"] = _settings.wifiSTAPassword;

    JsonArray tcpArr = doc["tcp_connections"].to<JsonArray>();
    for (auto& ep : _settings.tcpConnections) {
        JsonObject obj = tcpArr.add<JsonObject>();
        obj["host"] = ep.host;
        obj["port"] = ep.port;
        obj["auto"] = ep.autoConnect;
    }

    doc["screen_dim"] = _settings.screenDimTimeout;
    doc["screen_off"] = _settings.screenOffTimeout;
    doc["brightness"] = _settings.brightness;

    doc["audio_on"]  = _settings.audioEnabled;
    doc["audio_vol"] = _settings.audioVolume;

    doc["utc_offset"]   = _settings.utcOffset;
    doc["tz_idx"]       = _settings.timezoneIdx;
    doc["tz_set"]       = _settings.timezoneSet;
    doc["gps_time"]     = _settings.gpsTimeEnabled;
    doc["gps_loc"]      = _settings.gpsLocationEnabled;
    doc["display_name"] = _settings.displayName;

    String json;
    serializeJson(doc, json);
    return json;
}

// ---------------------------------------------------------------------------
// Flash-only (original API)
// ---------------------------------------------------------------------------

bool UserConfig::load(FlashStore& flash) {
    _migrationDirty = false;

    // Try flash file
    String json = flash.readString(PATH_USER_CONFIG);
    if (!json.isEmpty() && parseJson(json)) {
        if (_migrationDirty) save(flash);
        return true;
    }

    // Fall back to NVS
    json = loadFromNVS();
    if (!json.isEmpty()) {
        Serial.println("[CONFIG] Recovered from NVS (flash file missing)");
        if (parseJson(json)) {
            if (_migrationDirty) save(flash);
            return true;
        }
    }

    Serial.println("[CONFIG] No saved config anywhere, using defaults");
    return false;
}

bool UserConfig::save(FlashStore& flash) {
    String json = serializeToJson();
    bool ok = false;

    if (flash.writeString(PATH_USER_CONFIG, json)) {
        Serial.println("[CONFIG] Saved to flash");
        ok = true;
    }

    // Always save full config to NVS — bulletproof backup
    saveToNVS(json);

    return ok;
}

// ---------------------------------------------------------------------------
// Dual-backend: SD primary, flash fallback, NVS bulletproof
// ---------------------------------------------------------------------------

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    _migrationDirty = false;

    // Tier 1: SD card
    if (sd.isReady()) {
        String json = sd.readString(SD_PATH_USER_CONFIG);
        if (!json.isEmpty()) {
            Serial.println("[CONFIG] Loading from SD card");
            if (parseJson(json)) {
                if (_migrationDirty) save(sd, flash);
                return true;
            }
        }
    }

    // Tier 2: Flash (LittleFS)
    {
        String json = flash.readString(PATH_USER_CONFIG);
        if (!json.isEmpty()) {
            Serial.println("[CONFIG] Loading from flash");
            if (parseJson(json)) {
                // Auto-migrate to SD if available
                if (sd.isReady()) {
                    sd.ensureDir("/ratcom");
                    sd.ensureDir("/ratcom/config");
                    String migrateJson = serializeToJson();
                    sd.writeString(SD_PATH_USER_CONFIG, migrateJson);
                    Serial.println("[CONFIG] Migrated to SD");
                }
                if (_migrationDirty) save(sd, flash);
                return true;
            }
        }
    }

    // Tier 3: NVS (bulletproof — survives flash corruption)
    {
        String json = loadFromNVS();
        if (!json.isEmpty()) {
            Serial.println("[CONFIG] Recovered full config from NVS");
            if (parseJson(json)) {
                // Re-save to SD/flash to heal the corruption
                save(sd, flash);
                return true;
            }
        }
    }

    Serial.println("[CONFIG] No saved config anywhere, using defaults");
    return false;
}

bool UserConfig::save(SDStore& sd, FlashStore& flash) {
    String json = serializeToJson();
    bool ok = false;

    // Write to SD (primary)
    if (sd.isReady()) {
        sd.ensureDir("/ratcom");
        sd.ensureDir("/ratcom/config");
        if (sd.writeString(SD_PATH_USER_CONFIG, json)) {
            Serial.println("[CONFIG] Saved to SD");
            ok = true;
        } else {
            Serial.println("[CONFIG] SD write failed");
        }
    }

    // Write to flash (backup)
    if (flash.writeString(PATH_USER_CONFIG, json)) {
        Serial.println("[CONFIG] Saved to flash");
        ok = true;
    }

    // Always save full config to NVS — bulletproof backup
    // NVS is on internal flash, no SPI bus, wear-leveled, checksum-protected
    if (saveToNVS(json)) ok = true;

    return ok;
}
