#pragma once

// =============================================================================
// SX1262 LoRa Radio Driver — RadioLib Backend
//
// Same public interface as the original custom driver, but delegates all SPI
// and radio operations to RadioLib (proven compatible with RNode firmware).
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "RadioConstants.h"
#include "config/BoardConfig.h"

class RatLoRa {
public:
    RatLoRa(SPIClass* spi, int ss, int sclk, int mosi, int miso,
            int reset, int irq, int busy, int rxen = -1,
            bool tcxo = true, bool dio2_as_rf_switch = true);

    // --- Lifecycle ---
    bool begin(uint32_t frequency);
    void end();

    // --- TX ---
    int  beginPacket(int implicitHeader = 0);
    int  endPacket(bool async = false);
    bool isTxBusy();
    size_t write(uint8_t byte);
    size_t write(const uint8_t* buffer, size_t size);

    // --- RX ---
    void receive(int size = 0);
    int  available();
    int  read();
    int  peek();
    int  parsePacket(int size = 0);
    void readBytes(uint8_t* buffer, size_t size);

    // --- Configuration ---
    void setFrequency(uint32_t frequency);
    uint32_t getFrequency();
    void setTxPower(int level);
    int8_t getTxPower();
    void setSpreadingFactor(int sf);
    uint8_t getSpreadingFactor();
    void setSignalBandwidth(uint32_t sbw);
    uint32_t getSignalBandwidth();
    void setCodingRate4(int denominator);
    uint8_t getCodingRate4();
    void setPreambleLength(long length);
    void enableCrc();
    void disableCrc();

    // --- Status ---
    float getAirtime(uint16_t written);
    int  currentRssi();
    int  packetRssi();
    float packetSnr();
    bool isRadioOnline() { return _radioOnline; }
    long getPreambleLength() const { return _preambleLength; }
    uint8_t readRegister(uint16_t address);
    uint16_t getDeviceErrors();
    void clearDeviceErrors();
    uint8_t getStatus();
    uint16_t getIrqFlags();

    // --- Diagnostics ---
    void dumpRegisters(const char* label = "");
    int  runCADTest(int iterations = 20);

    // --- FIFO access ---
    void readBuffer(uint8_t* buffer, size_t size);
    const uint8_t* packetBuffer() const { return _packet; }

    // --- Interrupt-driven RX ---
    void onReceive(void(*callback)(int));

    // --- Power ---
    void standby();
    void sleep();

    // --- Misc ---
    uint8_t random();

public:
    volatile bool packetAvailable = false;

private:
    // --- RadioLib backend ---
    Module*       _mod = nullptr;
    ::SX1262*     _rl = nullptr;    // RadioLib SX1262 instance

    // --- Pin config (stored for RadioLib Module construction) ---
    SPIClass*   _spiModem;
    int _ss, _sclk, _mosi, _miso;
    int _reset, _irq, _busy, _rxen;
    bool _tcxo;
    bool _dio2_as_rf_switch;
    float _tcxoVoltage;

    // --- Cached parameters ---
    uint32_t _frequency = 0;
    uint8_t  _sf = 8;
    uint32_t _bwHz = 125000;
    uint8_t  _cr = 5;       // denominator (5 = 4/5)
    int8_t   _txp = 22;
    long     _preambleLength = 18;
    bool     _crcEnabled = true;
    bool     _radioOnline = false;

    // --- TX buffer (RadioLib needs all data at once) ---
    uint8_t  _txBuf[MAX_PACKET_SIZE] = {};
    int      _txBufLen = 0;
    bool     _txActive = false;
    uint32_t _txStartMs = 0;
    uint32_t _txTimeoutMs = 0;

    // --- RX packet buffer ---
    uint8_t  _packet[MAX_PACKET_SIZE] = {};
    int      _packetLen = 0;
    int      _packetIndex = 0;
    int      _lastRssi = 0;
    float    _lastSnr = 0;

    // --- ISR ---
    static void IRAM_ATTR onDio1Rise();
    static RatLoRa* _instance;
};
