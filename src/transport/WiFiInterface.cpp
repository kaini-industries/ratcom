#include "WiFiInterface.h"
#include "config/Config.h"

WiFiInterface::WiFiInterface(const char* name)
    : RNS::InterfaceImpl(name), _server(WIFI_AP_PORT)
{
    _IN = true;
    _OUT = true;
    _bitrate = 1000000;  // WiFi is fast
    _HW_MTU = 500;
    _apPassword = WIFI_AP_PASSWORD;
    _txBuffer = (uint8_t*)malloc(TX_BUFFER_SIZE);
    if (!_txBuffer) Serial.println("[WIFI] WARNING: TX buffer allocation failed");
}

WiFiInterface::~WiFiInterface() {
    stop();
    if (_txBuffer) { free(_txBuffer); _txBuffer = nullptr; }
}

void WiFiInterface::setAPCredentials(const char* ssid, const char* password) {
    _apSSID = ssid;
    _apPassword = password;
}

void WiFiInterface::setSTACredentials(const char* ssid, const char* password) {
    _staSSID = ssid;
    _staPassword = password;
}

bool WiFiInterface::isSTAConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

void WiFiInterface::startAP() {
    // Generate SSID from identity hash if not set
    if (_apSSID.isEmpty()) {
        uint32_t chip = ESP.getEfuseMac() & 0xFFFF;
        char ssid[32];
        snprintf(ssid, sizeof(ssid), "ratcom-%04x", chip);
        _apSSID = ssid;
    }

    // AP-only mode — saves ~20KB vs WIFI_AP_STA
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID.c_str(), _apPassword.c_str());

    Serial.printf("[WIFI] AP started: %s @ %s\n",
                  _apSSID.c_str(),
                  WiFi.softAPIP().toString().c_str());

    _server.begin();
    _apActive = true;
}

bool WiFiInterface::start() {
    startAP();
    // STA connection is handled separately in main.cpp (non-blocking)
    _online = true;
    return true;
}

void WiFiInterface::stop() {
    _online = false;
    _apActive = false;
    for (auto& client : _clients) {
        client.stop();
    }
    _clients.clear();
    _server.stop();
    WiFi.softAPdisconnect(true);
}

void WiFiInterface::stopFull() {
    stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Full shutdown");
}

void WiFiInterface::acceptClients() {
    WiFiClient newClient = _server.available();
    if (newClient) {
        _clients.push_back(newClient);
        Serial.printf("[WIFI] Client connected (%d total)\n", (int)_clients.size());
    }
}

void WiFiInterface::readClients() {
    for (int i = _clients.size() - 1; i >= 0; i--) {
        if (!_clients[i].connected()) {
            _clients[i].stop();
            _clients.erase(_clients.begin() + i);
            Serial.printf("[WIFI] Client disconnected (%d total)\n", (int)_clients.size());
            continue;
        }

        int len = readFrame(_clients[i], _rxBuffer, sizeof(_rxBuffer));
        if (len > 0) {
            RNS::Bytes data(_rxBuffer, len);
            Serial.printf("[WIFI] RX %d bytes from client\n", len);
            InterfaceImpl::handle_incoming(data);
        }
    }
}

void WiFiInterface::sendToClients(const uint8_t* data, size_t len) {
    for (auto& client : _clients) {
        if (client.connected()) {
            sendFrame(client, data, len);
        }
    }
}

void WiFiInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online) return;

    sendToClients(data.data(), data.size());
    Serial.printf("[WIFI] TX %d bytes to %d clients\n",
                  (int)data.size(), (int)_clients.size());
    InterfaceImpl::handle_outgoing(data);
}

void WiFiInterface::loop() {
    if (!_online) return;
    acceptClients();
    readClients();
}

// HDLC-like framing: [0x7E] [escaped data] [0x7E]
// Buffered write — single syscall instead of per-byte writes
void WiFiInterface::sendFrame(WiFiClient& client, const uint8_t* data, size_t len) {
    if (!_txBuffer) {
        // Fallback: per-byte writes if buffer allocation failed
        client.write(FRAME_START);
        for (size_t i = 0; i < len; i++) {
            if (data[i] == FRAME_START || data[i] == FRAME_ESC) {
                client.write(FRAME_ESC);
                client.write(data[i] ^ FRAME_XOR);
            } else {
                client.write(data[i]);
            }
        }
        client.write(FRAME_START);
        client.flush();
        return;
    }
    // Worst case: every byte escapes (2x) + 2 delimiters
    size_t maxFrameLen = len * 2 + 2;
    if (maxFrameLen > TX_BUFFER_SIZE) {
        Serial.printf("[WIFI] TX frame too large (%d bytes), dropping\n", (int)len);
        return;
    }
    size_t pos = 0;
    _txBuffer[pos++] = FRAME_START;
    for (size_t i = 0; i < len && pos < TX_BUFFER_SIZE - 2; i++) {
        if (data[i] == FRAME_START || data[i] == FRAME_ESC) {
            _txBuffer[pos++] = FRAME_ESC;
            _txBuffer[pos++] = data[i] ^ FRAME_XOR;
        } else {
            _txBuffer[pos++] = data[i];
        }
    }
    _txBuffer[pos++] = FRAME_START;
    client.write(_txBuffer, pos);
    // No flush() — TCP_NODELAY sends immediately without Nagle delay
}

int WiFiInterface::readFrame(WiFiClient& client, uint8_t* buffer, size_t maxLen) {
    if (!client.available()) return 0;

    bool inFrame = false;
    bool escaped = false;
    size_t pos = 0;

    // Tight drain loop: wait up to 10ms for complete frame (AP clients are on LAN)
    unsigned long deadline = millis() + 10;
    while (pos < maxLen) {
        if (!client.available()) {
            if (millis() >= deadline) break;
            delay(1);
            continue;
        }

        uint8_t b = client.read();

        if (b == FRAME_START) {
            if (inFrame && pos > 0) {
                return pos;  // End of frame
            }
            inFrame = true;
            pos = 0;
            continue;
        }

        if (!inFrame) continue;

        if (b == FRAME_ESC) {
            escaped = true;
            continue;
        }

        if (escaped) {
            buffer[pos++] = b ^ FRAME_XOR;
            escaped = false;
        } else {
            buffer[pos++] = b;
        }
    }

    return 0;  // Incomplete frame
}
