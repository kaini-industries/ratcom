#include "StatusBar.h"
#include "Theme.h"
#include <M5Unified.h>
#include <time.h>

void StatusBar::render(M5Canvas& canvas) {
    // Dark panel background
    canvas.fillRect(0, 0, Theme::SCREEN_W, Theme::STATUS_BAR_H, Theme::BAR_BG);
    canvas.drawFastHLine(0, Theme::STATUS_BAR_H - 1, Theme::SCREEN_W, Theme::BORDER);

    canvas.setTextSize(Theme::FONT_SIZE);

    // Left side: clock (if GPS time synced) or battery icon + %
    unsigned long now = millis();
    if (_smoothedBattery < 0 || now - _lastBatteryRead >= 2000) {
        int raw = M5.Power.getBatteryLevel();
        if (_smoothedBattery < 0) {
            _smoothedBattery = (float)raw;
        } else {
            _smoothedBattery = _smoothedBattery * 0.85f + (float)raw * 0.15f;
        }
        _lastBatteryRead = now;
    }
    int batLevel = (int)(_smoothedBattery + 0.5f);
    if (batLevel < 0) batLevel = 0;
    if (batLevel > 100) batLevel = 100;

    // Battery icon: always shown (compact)
    int bx = 2, by = 2, bw = 14, bh = 7;
    uint16_t batColor = Theme::PRIMARY;
    if (batLevel <= 10) batColor = Theme::ERROR;
    else if (batLevel <= 30) batColor = Theme::WARNING;
    canvas.drawRect(bx, by, bw, bh, batColor);
    canvas.fillRect(bx + bw, by + 2, 2, 3, batColor);
    int fillW = (bw - 2) * batLevel / 100;
    if (fillW > 0) canvas.fillRect(bx + 1, by + 1, fillW, bh - 2, batColor);

    int textX = bx + bw + 4;

    // Show clock if system time is valid (GPS or NTP), otherwise battery %
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm* tm = localtime(&t);
        char clockStr[8];
        snprintf(clockStr, sizeof(clockStr), "%d:%02d", tm->tm_hour, tm->tm_min);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(textX, Theme::STATUS_PAD);
        canvas.print(clockStr);
    } else {
        char batStr[6];
        snprintf(batStr, sizeof(batStr), "%d%%", batLevel);
        canvas.setTextColor(batColor);
        canvas.setCursor(textX, Theme::STATUS_PAD);
        canvas.print(batStr);
    }

    // Center text — flash "ANNOUNCED" briefly, otherwise show mode
    const char* centerText = _transportMode;
    uint16_t centerColor = Theme::ACCENT;
    if (millis() < _announceFlashUntil) {
        centerText = "ANNOUNCED";
        centerColor = Theme::PRIMARY;
    }
    canvas.setTextColor(centerColor);
    int modeLen = strlen(centerText) * Theme::CHAR_W;
    canvas.setCursor((Theme::SCREEN_W - modeLen) / 2, Theme::STATUS_PAD);
    canvas.print(centerText);

    // Connection indicators (right) — LoRa + TCP
    int rx = Theme::SCREEN_W - Theme::STATUS_PAD;

    // TCP indicator
    if (_tcpConnected) {
        const char* tcpStr = "TCP";
        int tcpW = strlen(tcpStr) * Theme::CHAR_W;
        rx -= tcpW;
        canvas.setTextColor(Theme::ACCENT);
        canvas.setCursor(rx, Theme::STATUS_PAD);
        canvas.print(tcpStr);
        rx -= 4;
    }

    // LoRa indicator — blink when unhealthy
    const char* loraStr = _loraOnline ? "LoRa" : "----";
    uint16_t loraColor = Theme::MUTED;
    if (_loraOnline) {
        if (_loraHealthy) {
            loraColor = Theme::PRIMARY;
        } else {
            // Blink red/yellow at 2Hz when radio is sick
            loraColor = ((millis() / 500) % 2) ? Theme::ERROR : Theme::WARNING;
        }
    }
    int loraW = strlen(loraStr) * Theme::CHAR_W;
    rx -= loraW;
    canvas.fillCircle(rx - 4, Theme::STATUS_PAD + 3, 2, loraColor);
    canvas.setTextColor(loraColor);
    canvas.setCursor(rx, Theme::STATUS_PAD);
    canvas.print(loraStr);
}
