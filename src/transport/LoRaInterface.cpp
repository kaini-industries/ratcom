#include "LoRaInterface.h"
#include "config/BoardConfig.h"
#include <algorithm>

// RNode on-air framing constants (from RNode_Firmware Framing.h / Config.h)
// Every LoRa packet has a 1-byte header: upper nibble = random sequence, lower nibble = flags
#define RNODE_HEADER_L      1
#define RNODE_FLAG_SPLIT    0x01
#define RNODE_NIBBLE_SEQ    0xF0
#define RNODE_SINGLE_MTU    (MAX_PACKET_SIZE - RNODE_HEADER_L)  // 254 bytes payload per frame

LoRaInterface::LoRaInterface(RatLoRa* radio, const char* name)
    : RNS::InterfaceImpl(name), _radio(radio)
{
    _IN = true;
    _OUT = true;
    _bitrate = 2000;
    // Reticulum MTU (500 bytes) — split-packet framing allows up to 2x254 = 508 bytes
    _HW_MTU = RNS::Type::Reticulum::MTU;
}

LoRaInterface::~LoRaInterface() {
    stop();
}

bool LoRaInterface::start() {
    if (!_radio || !_radio->isRadioOnline()) {
        Serial.println("[LORA_IF] Radio not available");
        _online = false;
        return false;
    }
    _online = true;
    _radio->receive();
    Serial.println("[LORA_IF] Interface started (split-packet enabled, MTU=500)");
    return true;
}

void LoRaInterface::stop() {
    _online = false;
    Serial.println("[LORA_IF] Interface stopped");
}

void LoRaInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online || !_radio) return;

    // Reject packets exceeding Reticulum MTU (500 bytes)
    if (data.size() > RNS::Type::Reticulum::MTU) {
        Serial.printf("[LORA_IF] TX DROPPED: exceeds Reticulum MTU (%d > %d)\n",
            (int)data.size(), (int)RNS::Type::Reticulum::MTU);
        return;
    }

    if (_txPending || _splitTxPending) {
        if ((int)_txQueue.size() < TX_QUEUE_MAX) {
            _txQueue.push_back(data);
            Serial.printf("[LORA_IF] TX queued (%d in queue)\n", (int)_txQueue.size());
        } else {
            Serial.println("[LORA_IF] TX queue full, dropping oldest");
            _txQueue.pop_front();
            _txQueue.push_back(data);
        }
        return;
    }

    transmitNow(data);
}

void LoRaInterface::transmitNow(const RNS::Bytes& data) {
    uint8_t header = (uint8_t)(random(256)) & RNODE_NIBBLE_SEQ;
    bool needsSplit = (data.size() > RNODE_SINGLE_MTU);

    if (needsSplit) {
        header |= RNODE_FLAG_SPLIT;
        size_t firstLen = RNODE_SINGLE_MTU;

        Serial.printf("[LORA_IF] TX SPLIT: %d bytes in 2 frames (seq=0x%02X)\n",
            (int)data.size(), header & RNODE_NIBBLE_SEQ);

        _radio->beginPacket();
        _radio->write(header);
        _radio->write(data.data(), firstLen);
        _radio->endPacket(true);

        // Save remaining data for second frame
        _splitTxPending = true;
        _splitTxRemaining = RNS::Bytes(data.data() + firstLen, data.size() - firstLen);
        _splitTxHeader = header;

        Serial.printf("[LORA_IF] TX SPLIT frame 1: %d+1 bytes (remaining: %d)\n",
            (int)firstLen, (int)_splitTxRemaining.size());
    } else {
        _radio->beginPacket();
        _radio->write(header);
        _radio->write(data.data(), data.size());
        _radio->endPacket(true);

        Serial.printf("[LORA_IF] TX %d+1 bytes (hdr=0x%02X)\n", (int)data.size(), header);
    }

    // Wire debug: decode Reticulum packet header for interop analysis
    if (_wireDebug && data.size() >= 2) {
        dumpPacketHeader("TX", data.data(), data.size());
    }

    _txPending = true;
    _txStartMs = millis();
    _txData = data;
    InterfaceImpl::handle_outgoing(data);

    // Track airtime
    size_t airBytes = needsSplit ? (RNODE_SINGLE_MTU + RNODE_HEADER_L) : (data.size() + RNODE_HEADER_L);
    float airtimeMs = _radio->getAirtime(airBytes);
    unsigned long txNow = millis();
    if (txNow - _airtimeWindowStart >= AIRTIME_WINDOW_MS) {
        _airtimeAccumMs = 0;
        _airtimeWindowStart = txNow;
    } else {
        float elapsed = (float)(txNow - _airtimeWindowStart);
        float remaining = 1.0f - (elapsed / AIRTIME_WINDOW_MS);
        if (remaining < 0) remaining = 0;
        _airtimeAccumMs *= remaining;
        _airtimeWindowStart = txNow;
    }
    _airtimeAccumMs += airtimeMs;
}

void LoRaInterface::loop() {
    if (!_online || !_radio) return;

    // Handle async TX completion
    if (_txPending) {
        if (millis() - _txStartMs > TX_TIMEOUT_MS) {
            _consecutiveTxTimeouts++;
            Serial.printf("[LORA_IF] TX TIMEOUT #%d — radio stuck, forcing recovery\n",
                          _consecutiveTxTimeouts);
            _txPending = false;
            _splitTxPending = false;
            _splitTxRemaining = RNS::Bytes();
            _txData = RNS::Bytes();

            // Auto-reinit after consecutive failures
            if (_consecutiveTxTimeouts >= REINIT_THRESHOLD) {
                Serial.printf("[LORA_IF] WATCHDOG: %d consecutive TX timeouts — reinitializing radio\n",
                              _consecutiveTxTimeouts);
                _totalReinits++;
                if (_radio->reinit()) {
                    _consecutiveTxTimeouts = 0;
                    Serial.println("[LORA_IF] WATCHDOG: radio reinitialized OK");
                } else {
                    Serial.println("[LORA_IF] WATCHDOG: reinit FAILED — radio may be dead");
                    _online = false;
                }
            }
            _radio->receive();
            return;
        }
        if (!_radio->isTxBusy()) {
            _txPending = false;
            _consecutiveTxTimeouts = 0;  // Successful TX — reset watchdog

            // If split TX pending, send the second frame immediately
            if (_splitTxPending) {
                _splitTxPending = false;

                Serial.printf("[LORA_IF] TX SPLIT frame 2: %d+1 bytes\n",
                    (int)_splitTxRemaining.size());

                _radio->beginPacket();
                _radio->write(_splitTxHeader);
                _radio->write(_splitTxRemaining.data(), _splitTxRemaining.size());
                _radio->endPacket(true);

                _txPending = true;
                size_t frame2Len = _splitTxRemaining.size();
                _splitTxRemaining = RNS::Bytes();

                float airtimeMs = _radio->getAirtime(frame2Len + RNODE_HEADER_L);
                _airtimeAccumMs += airtimeMs;
                return;
            }

            _txData = RNS::Bytes();

            if (!_txQueue.empty()) {
                RNS::Bytes next = _txQueue.front();
                _txQueue.pop_front();
                transmitNow(next);
            } else {
                _radio->receive();
            }
        }
        return;
    }

    // Split RX timeout: discard stale partial packets
    if (_splitRxPending && (millis() - _splitRxTimestamp > SPLIT_RX_TIMEOUT_MS)) {
        Serial.println("[LORA_IF] RX SPLIT timeout, discarding partial");
        _splitRxPending = false;
        _splitRxBuffer = RNS::Bytes();
    }

    // Periodic RX debug — IRQ flags, DIO1 state, device errors
    static unsigned long lastRxDebug = 0;
    if (millis() - lastRxDebug > 5000) {
        lastRxDebug = millis();
        uint16_t irq = _radio->getIrqFlags();
        uint16_t devErr = _radio->getDeviceErrors();
        int dio1 = digitalRead(LORA_IRQ);
        Serial.printf("[LORA_IF] RX: pktAvail=%d dio1=%d irq=0x%04X devErr=0x%04X\n",
                      _radio->packetAvailable ? 1 : 0, dio1, irq, devErr);
    }

    if (!_radio->packetAvailable) return;

    int packetSize = _radio->parsePacket();
    // Clear flag AFTER parsePacket — if ISR fires during readData(), we don't lose the new event
    _radio->packetAvailable = false;
    if (packetSize <= RNODE_HEADER_L) {
        if (packetSize > 0) {
            Serial.printf("[LORA_IF] RX runt packet (%d bytes), discarding\n", packetSize);
        }
        _radio->receive();
        return;
    }

    uint8_t raw[MAX_PACKET_SIZE];
    memcpy(raw, _radio->packetBuffer(), packetSize);

    // Capture signal quality before any further processing
    _lastRxRssi = _radio->packetRssi();
    _lastRxSnr = _radio->packetSnr();
    _lastRxTime = millis();

    uint8_t header = raw[0];
    int payloadSize = packetSize - RNODE_HEADER_L;
    uint8_t seq = header & RNODE_NIBBLE_SEQ;
    bool isSplit = (header & RNODE_FLAG_SPLIT) != 0;

    if (isSplit) {
        if (!_splitRxPending) {
            _splitRxPending = true;
            _splitRxSeq = seq;
            _splitRxBuffer = RNS::Bytes(raw + RNODE_HEADER_L, payloadSize);
            _splitRxTimestamp = millis();

            Serial.printf("[LORA_IF] RX SPLIT frame 1: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr);
            _radio->receive();
            return;
        } else if (seq == _splitRxSeq) {
            Serial.printf("[LORA_IF] RX SPLIT frame 2: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr);

            _splitRxBuffer.append(raw + RNODE_HEADER_L, payloadSize);
            int totalSize = _splitRxBuffer.size();
            _splitRxPending = false;

            Serial.printf("[LORA_IF] RX SPLIT reassembled: %d bytes total\n", totalSize);

            // Wire debug: decode reassembled Reticulum packet
            if (_wireDebug && _splitRxBuffer.size() >= 2) {
                dumpPacketHeader("RX", _splitRxBuffer.data(), _splitRxBuffer.size(), _lastRxRssi, _lastRxSnr);
            }

            InterfaceImpl::handle_incoming(_splitRxBuffer);
            _splitRxBuffer = RNS::Bytes();

            // Always re-arm RX — if TX is pending, transmitNow() will override
            _radio->receive();
            return;
        } else {
            Serial.printf("[LORA_IF] RX SPLIT seq mismatch (had 0x%02X, got 0x%02X), restarting\n",
                _splitRxSeq, seq);
            _splitRxSeq = seq;
            _splitRxBuffer = RNS::Bytes(raw + RNODE_HEADER_L, payloadSize);
            _splitRxTimestamp = millis();
            _radio->receive();
            return;
        }
    }

    if (_splitRxPending) {
        Serial.println("[LORA_IF] RX non-split while waiting for split frame 2, discarding partial");
        _splitRxPending = false;
        _splitRxBuffer = RNS::Bytes();
    }

    Serial.printf("[LORA_IF] RX %d bytes (hdr=0x%02X, payload=%d), RSSI=%d, SNR=%.1f\n",
                  packetSize, header, payloadSize,
                  _lastRxRssi, _lastRxSnr);

    RNS::Bytes buf(payloadSize);
    memcpy(buf.writable(payloadSize), raw + RNODE_HEADER_L, payloadSize);

    // Wire debug: decode Reticulum packet header for interop analysis
    if (_wireDebug && payloadSize >= 2) {
        dumpPacketHeader("RX", raw + RNODE_HEADER_L, payloadSize, _lastRxRssi, _lastRxSnr);
    }

    InterfaceImpl::handle_incoming(buf);

    // Always re-arm RX — if TX is pending, transmitNow() will override
    _radio->receive();
}

float LoRaInterface::airtimeUtilization() const {
    if (_airtimeAccumMs <= 0) return 0;
    unsigned long elapsed = millis() - _airtimeWindowStart;
    if (elapsed == 0) elapsed = 1;
    float windowMs = std::min((float)elapsed, (float)AIRTIME_WINDOW_MS);
    return _airtimeAccumMs / windowMs;
}

void LoRaInterface::dumpPacketHeader(const char* direction, const uint8_t* data, size_t len, int rssi, float snr) {
    if (len < 2) return;

    // Decode Reticulum header (first 2 bytes after RNode framing is stripped)
    uint8_t flags = data[0];
    uint8_t hops = data[1];
    int headerType   = (flags >> 6) & 0x01;
    int ifacFlag     = (flags >> 7) & 0x01;
    int contextFlag  = (flags >> 5) & 0x01;
    int transportType = (flags >> 4) & 0x01;
    int destType     = (flags >> 2) & 0x03;
    int pktType      = flags & 0x03;

    const char* pktTypeStr[] = {"DATA", "ANNOUNCE", "LINKREQ", "PROOF"};
    const char* destTypeStr[] = {"SINGLE", "GROUP", "PLAIN", "LINK"};
    const char* hdrTypeStr[] = {"H1(16B)", "H2(32B)"};

    Serial.printf("[WIRE-%s] %d bytes flags=0x%02X %s %s %s %s ifac=%d ctx=%d hops=%d",
        direction, (int)len, flags,
        pktTypeStr[pktType & 0x03], destTypeStr[destType & 0x03],
        hdrTypeStr[headerType & 0x01],
        transportType ? "TRANSPORT" : "BROADCAST",
        ifacFlag, contextFlag, hops);

    if (rssi != 0 || snr != 0) {
        Serial.printf(" rssi=%d snr=%.1f", rssi, snr);
    }

    // Show destination hash (starts at byte 2)
    if (len >= 18) {
        Serial.printf(" dest=");
        for (int i = 2; i < 18 && i < (int)len; i++) Serial.printf("%02x", data[i]);
    }

    // For HEADER_2, also show transport_id
    if (headerType == 1 && len >= 34) {
        Serial.printf(" via=");
        for (int i = 18; i < 34; i++) Serial.printf("%02x", data[i]);
    }

    Serial.println();

    // Hex dump of first 48 bytes
    int dumpLen = (len < 48) ? len : 48;
    Serial.printf("[WIRE-%s] hex: ", direction);
    for (int i = 0; i < dumpLen; i++) Serial.printf("%02x", data[i]);
    if ((int)len > dumpLen) Serial.printf("... (+%d)", (int)len - dumpLen);
    Serial.println();
}
