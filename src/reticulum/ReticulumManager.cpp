#include "ReticulumManager.h"
#include "config/Config.h"
#include <Log.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <map>
#include <unordered_map>
#include <string>

// =============================================================================
// LittleFS Filesystem Implementation for microReticulum
// =============================================================================

bool LittleFSFileSystem::init() {
    return true;  // LittleFS already initialized by FlashStore
}

bool LittleFSFileSystem::file_exists(const char* file_path) {
    return LittleFS.exists(file_path);
}

size_t LittleFSFileSystem::read_file(const char* file_path, RNS::Bytes& data) {
    File f = LittleFS.open(file_path, "r");
    if (!f) return 0;
    size_t size = f.size();
    data = RNS::Bytes(size);
    f.readBytes((char*)data.writable(size), size);
    f.close();
    return size;
}

size_t LittleFSFileSystem::write_file(const char* file_path, const RNS::Bytes& data) {
    // Ensure parent directory exists
    String path = String(file_path);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!LittleFS.exists(dir.c_str())) {
            LittleFS.mkdir(dir.c_str());
        }
    }

    File f = LittleFS.open(file_path, "w");
    if (!f) return 0;
    size_t written = f.write(data.data(), data.size());
    f.close();
    return written;
}

RNS::FileStream LittleFSFileSystem::open_file(const char* file_path, RNS::FileStream::MODE file_mode) {
    return {RNS::Type::NONE};
}

bool LittleFSFileSystem::remove_file(const char* file_path) {
    return LittleFS.remove(file_path);
}

bool LittleFSFileSystem::rename_file(const char* from, const char* to) {
    return LittleFS.rename(from, to);
}

bool LittleFSFileSystem::directory_exists(const char* directory_path) {
    return LittleFS.exists(directory_path);
}

bool LittleFSFileSystem::create_directory(const char* directory_path) {
    return LittleFS.mkdir(directory_path);
}

bool LittleFSFileSystem::remove_directory(const char* directory_path) {
    return LittleFS.rmdir(directory_path);
}

std::list<std::string> LittleFSFileSystem::list_directory(const char* directory_path, Callbacks::DirectoryListing callback) {
    std::list<std::string> entries;
    File dir = LittleFS.open(directory_path);
    if (!dir || !dir.isDirectory()) return entries;
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
        entries.push_back(name);
        if (callback) callback(name);
        f = dir.openNextFile();
    }
    return entries;
}

size_t LittleFSFileSystem::storage_size() {
    return LittleFS.totalBytes();
}

size_t LittleFSFileSystem::storage_available() {
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

// =============================================================================
// ReticulumManager
// =============================================================================

bool ReticulumManager::begin(SX1262* radio, FlashStore* flash) {
    _flash = flash;

    // Register filesystem with microReticulum (required — library throws if missing)
    LittleFSFileSystem* fsImpl = new LittleFSFileSystem();
    RNS::FileSystem fs(fsImpl);
    fs.init();
    RNS::Utilities::OS::register_filesystem(fs);
    Serial.println("[RNS] Filesystem registered");

    // Restore routing tables and known destinations from SD if missing on flash
    if (_sd && _sd->isReady()) {
        static const char* files[] = {"/destination_table", "/packet_hashlist", "/known_destinations"};
        for (const char* name : files) {
            if (!LittleFS.exists(name)) {
                char sdPath[64];
                snprintf(sdPath, sizeof(sdPath), "/ratcom/transport%s", name);
                uint8_t* buf = (uint8_t*)malloc(4096);
                if (!buf) { Serial.println("[RNS] SD restore: malloc failed"); continue; }
                size_t len = 0;
                if (_sd->readFile(sdPath, buf, 4096, len) && len > 0) {
                    File f = LittleFS.open(name, "w");
                    if (f) { f.write(buf, len); f.close(); }
                    Serial.printf("[RNS] Restored %s from SD (%d bytes)\n", name, (int)len);
                }
                free(buf);
            }
        }
    }

    // Create and register LoRa interface
    _loraImpl = new LoRaInterface(radio, "LoRa.915");
    _loraIface = _loraImpl;
    _loraIface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(_loraIface);
    if (!_loraImpl->start()) {
        Serial.println("[RNS] WARNING: LoRa interface failed to start — radio offline");
    }
    Serial.println("[RNS] LoRa interface registered");

    // Create Reticulum instance (endpoint only — no transport/rebroadcast)
    _reticulum = RNS::Reticulum();
    // Suppress verbose microReticulum logging — LOG_TRACE floods serial at 115200 baud,
    // blocking the CPU for hundreds of ms. Change to LOG_TRACE or LOG_DEBUG for protocol debugging.
    RNS::loglevel(RNS::LOG_WARNING);
    RNS::Reticulum::transport_enabled(false);
    RNS::Reticulum::probe_destination_enabled(true);
    // Cap table sizes — raised for 100+ node hubs while staying within ESP32 RAM
    RNS::Transport::path_table_maxsize(256);
    RNS::Transport::announce_table_maxsize(128);
    _reticulum.start();
    Serial.println("[RNS] Reticulum started (Endpoint)");

    // Layer 1: Transport-level announce filter — runs BEFORE Ed25519 verify
    RNS::Transport::set_filter_packet_callback([](const RNS::Packet& packet) -> bool {
        if (packet.packet_type() != RNS::Type::Packet::ANNOUNCE) return true;

        unsigned long now = millis();

        // Rate limit window (per-second)
        static unsigned long windowStart = 0;
        static unsigned int count = 0;
        if (now - windowStart >= 1000) { windowStart = now; count = 0; }

        // Adaptive rate: tighter during first 60s boot flood, then normal
        unsigned int maxRate = (now < 60000) ? 3 : RATCOM_MAX_ANNOUNCES_PER_SEC;
        if (++count > maxRate) return false;

        // Skip re-validation of known paths (saves ~100ms Ed25519 per announce)
        // Allow through once per 5 min for name/ratchet updates.
        // Uses raw hash bytes as key (avoids expensive toHex + hops_to per packet).
        if (RNS::Transport::has_path(packet.destination_hash())) {
            static std::unordered_map<std::string, unsigned long> lastRevalidate;
            std::string key((const char*)packet.destination_hash().data(),
                            packet.destination_hash().size());

            auto it = lastRevalidate.find(key);
            if (it != lastRevalidate.end() && (now - it->second) < 300000) return false;
            lastRevalidate[key] = now;

            // Cap map size to prevent unbounded growth
            if (lastRevalidate.size() > 300) lastRevalidate.clear();
        }

        return true;
    });

    // Load persisted known destinations so Identity::recall() works
    // immediately after reboot for previously-seen nodes.
    RNS::Identity::load_known_destinations();

    // Load or create identity
    if (!loadOrCreateIdentity()) {
        Serial.println("[RNS] ERROR: Identity creation failed!");
        return false;
    }

    // Create LXMF delivery destination
    _destination = RNS::Destination(
        _identity,
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );
    _destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
    _destination.accepts_links(true);
    Serial.printf("[RNS] Destination: %s\n", _destination.hash().toHex().c_str());

    _transportActive = true;
    startPersistTask();
    Serial.println("[RNS] Endpoint active");
    return true;
}

bool ReticulumManager::loadOrCreateIdentity() {
    // Tier 1: Flash (LittleFS)
    if (_flash->exists(PATH_IDENTITY)) {
        uint8_t keyBuf[128];
        size_t keyLen = 0;
        if (_flash->readFile(PATH_IDENTITY, keyBuf, sizeof(keyBuf), keyLen) && keyLen > 0) {
            RNS::Bytes keyData(keyBuf, keyLen);
            _identity = RNS::Identity(false);
            if (_identity.load_private_key(keyData)) {
                Serial.printf("[RNS] Identity loaded from flash: %s\n", _identity.hexhash().c_str());
                saveIdentityToAll(keyData);
                return true;
            }
        }
        Serial.println("[RNS] Failed to load identity from flash");
    }

    // Tier 2: NVS (ESP32 Preferences — always available, higher trust than SD)
    {
        Preferences prefs;
        if (prefs.begin("ratcom_id", true)) {
            size_t keyLen = prefs.getBytesLength("privkey");
            if (keyLen > 0 && keyLen <= 128) {
                uint8_t keyBuf[128];
                prefs.getBytes("privkey", keyBuf, keyLen);
                prefs.end();
                RNS::Bytes keyData(keyBuf, keyLen);
                _identity = RNS::Identity(false);
                if (_identity.load_private_key(keyData)) {
                    Serial.printf("[RNS] Identity restored from NVS: %s\n", _identity.hexhash().c_str());
                    saveIdentityToAll(keyData);
                    return true;
                }
            } else {
                prefs.end();
            }
        }
    }

    // Tier 3: SD card (lowest trust — may contain another device's identity)
    if (_sd && _sd->isReady() && _sd->exists(SD_PATH_IDENTITY)) {
        uint8_t keyBuf[128];
        size_t keyLen = 0;
        if (_sd->readFile(SD_PATH_IDENTITY, keyBuf, sizeof(keyBuf), keyLen) && keyLen > 0) {
            RNS::Bytes keyData(keyBuf, keyLen);
            _identity = RNS::Identity(false);
            if (_identity.load_private_key(keyData)) {
                Serial.printf("[RNS] Identity restored from SD: %s\n", _identity.hexhash().c_str());
                saveIdentityToAll(keyData);
                return true;
            }
        }
        Serial.println("[RNS] SD identity exists but failed to load");
    }

    // No identity found anywhere — create new
    _identity = RNS::Identity();
    Serial.printf("[RNS] New identity created: %s\n", _identity.hexhash().c_str());

    RNS::Bytes privKey = _identity.get_private_key();
    if (privKey.size() > 0) {
        saveIdentityToAll(privKey);
    }
    return true;
}

void ReticulumManager::saveIdentityToAll(const RNS::Bytes& keyData) {
    // Flash
    _flash->writeAtomic(PATH_IDENTITY, keyData.data(), keyData.size());
    // SD
    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratcom/identity");
        _sd->writeAtomic(SD_PATH_IDENTITY, keyData.data(), keyData.size());
    }
    // NVS (always available)
    Preferences prefs;
    if (prefs.begin("ratcom_id", false)) {
        prefs.putBytes("privkey", keyData.data(), keyData.size());
        prefs.end();
        Serial.println("[RNS] Identity saved to NVS");
    }
}

void ReticulumManager::loop() {
    if (!_transportActive) return;

    _reticulum.loop();
    if (_loraImpl) {
        _loraImpl->loop();
    }

    unsigned long now = millis();
    if (now - _lastPersist >= PATH_PERSIST_INTERVAL_MS) {
        _lastPersist = now;
        persistData();
    }
}

// --- Background persist task (runs on core 0) ---
// Flash writes take 0.5-4+ seconds on LittleFS. Running them on core 0
// keeps the main loop (core 1) responsive for UI and radio.

void ReticulumManager::startPersistTask() {
    _persistQueue = xQueueCreate(1, sizeof(uint8_t));
    // Pin to core 1 (same as main loop) — core 0 is WiFi/lwIP, sharing LittleFS across cores corrupts FS
    xTaskCreatePinnedToCore(persistTaskFunc, "persist", 8192, this, 1, &_persistTask, 1);
    Serial.println("[RNS] Persist task started on core 1");
}

void ReticulumManager::persistTaskFunc(void* param) {
    ReticulumManager* self = (ReticulumManager*)param;
    uint8_t cycle;
    for (;;) {
        if (xQueueReceive(self->_persistQueue, &cycle, portMAX_DELAY) == pdTRUE) {
            unsigned long start = millis();
            switch (cycle) {
                case 0:
                    RNS::Transport::persist_data();
                    break;
                case 1:
                    RNS::Identity::persist_data();
                    break;
                case 2:
                    if (self->_sd && self->_sd->isReady()) {
                        static const char* files[] = {"/destination_table", "/packet_hashlist", "/known_destinations"};
                        for (const char* name : files) {
                            File f = LittleFS.open(name, "r");
                            if (f && f.size() > 0) {
                                size_t sz = f.size();
                                uint8_t* buf = (uint8_t*)malloc(sz);
                                if (buf) {
                                    f.readBytes((char*)buf, sz);
                                    char sdPath[64];
                                    snprintf(sdPath, sizeof(sdPath), "/ratcom/transport%s", name);
                                    self->_sd->ensureDir("/ratcom/transport");
                                    self->_sd->writeDirect(sdPath, buf, sz);
                                    free(buf);
                                }
                            }
                            if (f) f.close();
                        }
                    }
                    break;
            }
            Serial.printf("[PERSIST] Cycle %d done (%lums, core %d)\n",
                          cycle, millis() - start, xPortGetCoreID());
        }
    }
}

void ReticulumManager::persistData() {
    if (_persistQueue) {
        // Queue the cycle for background execution — non-blocking
        xQueueOverwrite(_persistQueue, &_persistCycle);
    } else {
        // Fallback: synchronous (before task is started)
        switch (_persistCycle) {
            case 0: RNS::Transport::persist_data(); break;
            case 1: RNS::Identity::persist_data(); break;
        }
    }
    _persistCycle = (_persistCycle + 1) % 3;
}

String ReticulumManager::identityHash() const {
    if (!_identity) return "unknown";
    std::string hex = _identity.hexhash();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 4) + ":" + hex.substr(4, 4) + ":" + hex.substr(8, 4)).c_str());
    }
    return String(hex.c_str());
}

String ReticulumManager::destinationHashStr() const {
    if (!_destination) return "unknown";
    std::string hex = _destination.hash().toHex();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 4) + ":" + hex.substr(4, 4) + ":" + hex.substr(8, 4)).c_str());
    }
    return String(hex.c_str());
}

size_t ReticulumManager::pathCount() const {
    return _reticulum.get_path_table().size();
}

size_t ReticulumManager::linkCount() const {
    return _reticulum.get_link_count();
}

void ReticulumManager::announce(const RNS::Bytes& appData) {
    if (!_transportActive) return;
    Serial.printf("[TX-DBG] dest_hash:     %s\n", _destination.hash().toHex().c_str());
    Serial.printf("[TX-DBG] identity_hash: %s\n", _identity.hexhash().c_str());
    Serial.printf("[TX-DBG] public_key:    %s\n", _identity.get_public_key().toHex().c_str());
    RNS::Bytes nh = RNS::Identity::full_hash(RNS::Bytes("lxmf.delivery")).left(10);
    Serial.printf("[TX-DBG] name_hash:     %s\n", nh.toHex().c_str());
    RNS::Bytes hm = nh + _identity.hash();
    Serial.printf("[TX-DBG] hash_material: %s\n", hm.toHex().c_str());
    RNS::Bytes eh = RNS::Identity::full_hash(hm).left(16);
    Serial.printf("[TX-DBG] recomputed:    %s\n", eh.toHex().c_str());
    _destination.announce(appData);
    _lastAnnounceTime = millis();
    Serial.println("[RNS] Announce sent");
}
