// =============================================================================
// SX1262 LoRa Radio Driver — RadioLib Backend
//
// Wraps RadioLib's SX1262 implementation to provide the same API that
// LoRaInterface.cpp and main.cpp expect. RadioLib handles all SPI operations,
// register configuration, errata workarounds, and calibration.
//
// Why: The original custom driver (extracted from RNode_Firmware_CE) produced
// LoRa packets that RNode receivers couldn't demodulate, despite matching
// parameters. RadioLib's TX/RX sequence is proven compatible with RNode.
// =============================================================================

#include "SX1262.h"
#include "config/BoardConfig.h"

RatLoRa* RatLoRa::_instance = nullptr;

// =============================================================================
// Constructor
// =============================================================================

RatLoRa::RatLoRa(SPIClass* spi, int ss, int sclk, int mosi, int miso,
               int reset, int irq, int busy, int rxen,
               bool tcxo, bool dio2_as_rf_switch)
    : _spiModem(spi), _ss(ss), _sclk(sclk), _mosi(mosi), _miso(miso),
      _reset(reset), _irq(irq), _busy(busy), _rxen(rxen),
      _tcxo(tcxo), _dio2_as_rf_switch(dio2_as_rf_switch)
{
    _tcxoVoltage = tcxo ? 3.0f : 0.0f;
    _instance = this;
}

// =============================================================================
// ISR — sets packetAvailable flag for polling in LoRaInterface::loop()
// =============================================================================

void IRAM_ATTR RatLoRa::onDio1Rise() {
    if (_instance) {
        _instance->packetAvailable = true;
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

bool RatLoRa::begin(uint32_t frequency) {
    _frequency = frequency;

    // Start SPI bus
    _spiModem->begin(_sclk, _miso, _mosi, _ss);

    // Create RadioLib Module + SX1262 instance
    _mod = new Module(_ss, _irq, _reset, _busy, *_spiModem);
    _rl  = new ::SX1262(_mod);

    // Initialize with default parameters
    float freqMHz = (float)frequency / 1e6f;
    float bwKHz   = (float)_bwHz / 1e3f;
    int16_t state = _rl->begin(freqMHz, bwKHz, _sf, _cr, SYNC_WORD_6X, _txp, _preambleLength, _tcxoVoltage);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SX1262] RadioLib begin() failed: %d\n", state);
        return false;
    }

    // Enable DIO2 as RF switch (required for Cap LoRa-1262)
    if (_dio2_as_rf_switch) {
        _rl->setDio2AsRfSwitch(true);
    }

    // Enable CRC
    _rl->setCRC(true);
    _crcEnabled = true;

    // Boosted RX gain — full sensitivity (+3dB over power-saving default)
    _rl->setRxBoostedGainMode(true);

    // Set up DIO1 interrupt for RX packet detection
    _rl->setDio1Action(onDio1Rise);

    // Post-init diagnostics
    uint16_t devErr = getDeviceErrors();
    Serial.printf("[SX1262] RadioLib init OK (DevErrors: 0x%04X)\n", devErr);

    _radioOnline = true;
    return true;
}

void RatLoRa::end() {
    if (_rl) {
        _rl->sleep();
    }
    _radioOnline = false;
}

// =============================================================================
// TX Operations — buffer locally, then use RadioLib startTransmit
// =============================================================================

int RatLoRa::beginPacket(int implicitHeader) {
    _txBufLen = 0;
    memset(_txBuf, 0, sizeof(_txBuf));
    return 1;
}

size_t RatLoRa::write(uint8_t byte) {
    if (_txBufLen < MAX_PACKET_SIZE) {
        _txBuf[_txBufLen++] = byte;
        return 1;
    }
    return 0;
}

size_t RatLoRa::write(const uint8_t* buffer, size_t size) {
    size_t written = 0;
    for (size_t i = 0; i < size && _txBufLen < MAX_PACKET_SIZE; i++) {
        _txBuf[_txBufLen++] = buffer[i];
        written++;
    }
    return written;
}

int RatLoRa::endPacket(bool async) {
    if (!_rl || !_radioOnline) return 0;

    _txStartMs = millis();
    _txTimeoutMs = _txStartMs + (uint32_t)(getAirtime(_txBufLen) * 1.5f) + 2000;

    if (async) {
        // Non-blocking: use startTransmit, poll with isTxBusy()
        int16_t state = _rl->startTransmit(_txBuf, _txBufLen);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("[SX1262] startTransmit failed: %d\n", state);
            return 0;
        }
        _txActive = true;
        return 1;
    } else {
        // Blocking: use transmit
        int16_t state = _rl->transmit(_txBuf, _txBufLen);
        if (state == RADIOLIB_ERR_NONE) {
            return 1;
        }
        Serial.printf("[SX1262] transmit failed: %d\n", state);
        return 0;
    }
}

bool RatLoRa::isTxBusy() {
    if (!_txActive) return false;

    // Timeout check
    if (millis() > _txTimeoutMs) {
        Serial.printf("[SX1262] TX ASYNC TIMEOUT after %dms\n", (int)(millis() - _txStartMs));
        _rl->finishTransmit();
        _txActive = false;
        packetAvailable = false;
        return false;
    }

    // Check if DIO1 fired (TX_DONE)
    if (_mod->hal->digitalRead(_irq)) {
        _rl->finishTransmit();
        _txActive = false;
        // Clear stale flag — DIO1 fired for TX_DONE, not RX_DONE
        packetAvailable = false;
        return false;
    }

    return true;  // Still transmitting
}

// =============================================================================
// RX Operations — use RadioLib startReceive + interrupt
// =============================================================================

void RatLoRa::receive(int size) {
    if (!_rl) return;

    packetAvailable = false;

    // Re-register DIO1 interrupt — RadioLib's stageMode(TX) reconfigures IRQs for TX_DONE,
    // so we must re-arm for RX_DONE when switching back to receive.
    _rl->setDio1Action(onDio1Rise);

    int16_t state = _rl->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SX1262] startReceive failed: %d\n", state);
    }
}

int RatLoRa::parsePacket(int size) {
    if (!_rl) return 0;

    // Read received data via RadioLib
    _packetLen = 0;
    _packetIndex = 0;

    int16_t state = _rl->readData(_packet, 0);  // 0 = read whatever is available

    if (state == RADIOLIB_ERR_NONE) {
        _packetLen = _rl->getPacketLength();
        _lastRssi = (int)_rl->getRSSI();
        _lastSnr  = _rl->getSNR();
        return _packetLen;
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        _packetLen = _rl->getPacketLength();
        Serial.printf("[SX1262] RX CRC FAIL: %d bytes RSSI=%d SNR=%.1f\n",
                      _packetLen, (int)_rl->getRSSI(), _rl->getSNR());
        // Restart RX
        _rl->startReceive();
        return 0;
    } else {
        // Other error — restart RX
        _rl->startReceive();
        return 0;
    }
}

int RatLoRa::available() {
    return _packetLen - _packetIndex;
}

int RatLoRa::read() {
    if (_packetIndex < _packetLen) {
        return _packet[_packetIndex++];
    }
    return -1;
}

int RatLoRa::peek() {
    if (_packetIndex < _packetLen) {
        return _packet[_packetIndex];
    }
    return -1;
}

void RatLoRa::readBytes(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size && _packetIndex < _packetLen; i++) {
        buffer[i] = _packet[_packetIndex++];
    }
}

void RatLoRa::readBuffer(uint8_t* buffer, size_t size) {
    memcpy(buffer, _packet, min(size, (size_t)_packetLen));
}

// =============================================================================
// Configuration — delegate to RadioLib
// =============================================================================

void RatLoRa::setFrequency(uint32_t frequency) {
    _frequency = frequency;
    if (_rl) _rl->setFrequency((float)frequency / 1e6f);
}

uint32_t RatLoRa::getFrequency() {
    return _frequency;
}

void RatLoRa::setTxPower(int level) {
    if (level > 22) level = 22;
    if (level < -9) level = -9;
    _txp = level;
    if (_rl) _rl->setOutputPower(level);
}

int8_t RatLoRa::getTxPower() {
    return _txp;
}

void RatLoRa::setSpreadingFactor(int sf) {
    if (sf < 5) sf = 5;
    if (sf > 12) sf = 12;
    _sf = sf;
    if (_rl) _rl->setSpreadingFactor(sf);
}

uint8_t RatLoRa::getSpreadingFactor() {
    return _sf;
}

void RatLoRa::setSignalBandwidth(uint32_t sbw) {
    _bwHz = sbw;
    if (_rl) _rl->setBandwidth((float)sbw / 1e3f);
}

uint32_t RatLoRa::getSignalBandwidth() {
    return _bwHz;
}

void RatLoRa::setCodingRate4(int denominator) {
    if (denominator < 5) denominator = 5;
    if (denominator > 8) denominator = 8;
    _cr = denominator;
    if (_rl) _rl->setCodingRate(denominator);
}

uint8_t RatLoRa::getCodingRate4() {
    return _cr;
}

void RatLoRa::setPreambleLength(long length) {
    _preambleLength = length;
    if (_rl) _rl->setPreambleLength(length);
}

void RatLoRa::enableCrc() {
    _crcEnabled = true;
    if (_rl) _rl->setCRC(true);
}

void RatLoRa::disableCrc() {
    _crcEnabled = false;
    if (_rl) _rl->setCRC(false);
}

// =============================================================================
// Status
// =============================================================================

int RatLoRa::currentRssi() {
    if (!_rl) return -292;
    return (int)_rl->getRSSI();
}

int RatLoRa::packetRssi() {
    return _lastRssi;
}

float RatLoRa::packetSnr() {
    return _lastSnr;
}

float RatLoRa::getAirtime(uint16_t written) {
    if (!_rl || !_radioOnline) return 0;
    return _rl->getTimeOnAir(written) / 1000.0f;  // RadioLib returns microseconds, we want ms
}

uint8_t RatLoRa::readRegister(uint16_t address) {
    if (!_mod) return 0;
    return _mod->SPIreadRegister(address);
}

uint16_t RatLoRa::getDeviceErrors() {
    if (!_mod) return 0;
    uint8_t data[2] = {0, 0};
    _mod->SPIreadStream(RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS, data, 2);
    return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

void RatLoRa::clearDeviceErrors() {
    if (!_mod) return;
    uint8_t data[2] = {0, 0};
    _mod->SPIwriteStream(RADIOLIB_SX126X_CMD_CLEAR_DEVICE_ERRORS, data, 2);
}

uint8_t RatLoRa::getStatus() {
    if (!_mod) return 0;
    // Read status via NOP command
    return _mod->SPIgetRegValue(RADIOLIB_SX126X_REG_HOPPING_ENABLE, 7, 0);
}

uint16_t RatLoRa::getIrqFlags() {
    if (!_rl) return 0;
    return _rl->getIrqFlags();
}

// =============================================================================
// Diagnostics
// =============================================================================

void RatLoRa::dumpRegisters(const char* label) {
    uint8_t swMsb = readRegister(REG_SYNC_WORD_MSB_6X);
    uint8_t swLsb = readRegister(REG_SYNC_WORD_LSB_6X);
    uint8_t iqReg = readRegister(REG_IQ_POLARITY_6X);
    uint8_t lna   = readRegister(REG_LNA_6X);
    uint16_t devErr = getDeviceErrors();
    uint8_t status = getStatus();
    uint8_t chipMode = (status >> 4) & 0x07;

    Serial.printf("[SX1262] ┌─ Register Dump: %s ─┐\n", label);
    Serial.printf("[SX1262] │  SyncWord  = 0x%02X%02X\n", swMsb, swLsb);
    Serial.printf("[SX1262] │  IQ Polar  = 0x%02X (bit2=%d)\n", iqReg, (iqReg >> 2) & 1);
    Serial.printf("[SX1262] │  LNA       = 0x%02X\n", lna);
    Serial.printf("[SX1262] │  Params    = SF=%d BW=%lu CR=%d pre=%ld crc=%d\n",
                  _sf, (unsigned long)_bwHz, _cr, _preambleLength, _crcEnabled);
    Serial.printf("[SX1262] │  DevErrors = 0x%04X\n", devErr);
    Serial.printf("[SX1262] │  Status    = 0x%02X (mode=%d)\n", status, chipMode);
    Serial.println("[SX1262] └──────────────────────────────────────┘");
}

// =============================================================================
// CAD (Channel Activity Detection) — diagnostic for RX chain verification
// =============================================================================

int RatLoRa::runCADTest(int iterations) {
    if (!_rl) return -1;
    int detected = 0;
    for (int i = 0; i < iterations; i++) {
        int16_t result = _rl->scanChannel();
        if (result == RADIOLIB_LORA_DETECTED) {
            detected++;
            Serial.printf("[CAD] %d/%d: DETECTED\n", i + 1, iterations);
        } else if (result == RADIOLIB_CHANNEL_FREE) {
            Serial.printf("[CAD] %d/%d: free\n", i + 1, iterations);
        } else {
            Serial.printf("[CAD] %d/%d: error %d\n", i + 1, iterations, result);
        }
    }
    Serial.printf("[CAD] Result: %d/%d detected\n", detected, iterations);
    return detected;
}

// =============================================================================
// Interrupt-driven RX (legacy API — DIO1 ISR is set up in begin())
// =============================================================================

void RatLoRa::onReceive(void(*callback)(int)) {
    // Not used with RadioLib backend — DIO1 ISR handles packetAvailable
}

// =============================================================================
// Power Management
// =============================================================================

void RatLoRa::standby() {
    if (_rl) _rl->standby();
}

void RatLoRa::sleep() {
    if (_rl) _rl->sleep();
}

// =============================================================================
// Reinit — recover from stuck radio states without full reboot
// =============================================================================

bool RatLoRa::reinit() {
    Serial.println("[SX1262] REINIT: resetting radio...");

    if (_rl) {
        _rl->sleep();
        delay(10);
    }

    // Hardware reset via pin
    if (_reset >= 0) {
        pinMode(_reset, OUTPUT);
        digitalWrite(_reset, LOW);
        delay(10);
        digitalWrite(_reset, HIGH);
        delay(50);
    }

    // Re-initialize with cached parameters
    float freqMHz = (float)_frequency / 1e6f;
    float bwKHz   = (float)_bwHz / 1e3f;
    int16_t state = _rl->begin(freqMHz, bwKHz, _sf, _cr, SYNC_WORD_6X, _txp, _preambleLength, _tcxoVoltage);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SX1262] REINIT FAILED: begin() returned %d\n", state);
        _radioOnline = false;
        return false;
    }

    if (_dio2_as_rf_switch) {
        _rl->setDio2AsRfSwitch(true);
    }
    _rl->setCRC(_crcEnabled);
    _rl->setRxBoostedGainMode(true);
    _rl->setDio1Action(onDio1Rise);

    _txActive = false;
    packetAvailable = false;

    uint16_t devErr = getDeviceErrors();
    Serial.printf("[SX1262] REINIT OK (DevErrors: 0x%04X)\n", devErr);
    _radioOnline = true;
    return true;
}

// =============================================================================
// Misc
// =============================================================================

uint8_t RatLoRa::random() {
    if (!_rl) return 0;
    return (uint8_t)(_rl->randomByte());
}
