#include "HomeScreen.h"
#include "ui/Theme.h"
#include "config/Config.h"

static const char* detectPresetName(const UserSettings& s) {
    if (s.loraSF == 9 && s.loraBW == 125000 && s.loraCR == 5 && s.loraTxPower == 17)
        return "Balanced";
    if (s.loraSF == 12 && s.loraBW == 62500 && s.loraCR == 8 && s.loraTxPower == 22)
        return "Long Range";
    if (s.loraSF == 7 && s.loraBW == 250000 && s.loraCR == 5 && s.loraTxPower == 14)
        return "Fast";
    return "Custom";
}

bool HomeScreen::handleKey(const KeyEvent& event) {
    if (event.enter) {
        if (_announceCb) _announceCb();
        _announceFlashUntil = millis() + 1500;
        return true;
    }
    return false;
}

void HomeScreen::render(M5Canvas& canvas) {
    int y = Theme::CONTENT_Y + 2;
    int lineH = Theme::CHAR_H + 3;
    int pad = 4;
    canvas.setTextSize(Theme::FONT_SIZE);

    // === Identity card ===
    int cardY = y;
    int cardH = lineH * 2 + 6;
    canvas.drawRoundRect(2, cardY, Theme::SCREEN_W - 4, cardH, 3, Theme::BORDER);

    y = cardY + 3;
    // Display name
    canvas.setTextColor(Theme::ACCENT);
    canvas.setCursor(pad + 2, y);
    if (_userConfig && !_userConfig->settings().displayName.isEmpty()) {
        canvas.print(_userConfig->settings().displayName.c_str());
    } else {
        canvas.setTextColor(Theme::MUTED);
        canvas.print("(no name set)");
    }
    y += lineH;

    // LXMF hash
    canvas.setTextColor(Theme::SECONDARY);
    canvas.setCursor(pad + 2, y);
    canvas.print("LXMF ");
    canvas.setTextColor(Theme::PRIMARY);
    if (_rns) {
        canvas.print(_rns->destinationHashStr());
    }
    y += lineH + 4;

    // === Radio card ===
    cardY = y;
    cardH = lineH * 2 + 6;
    canvas.drawRoundRect(2, cardY, Theme::SCREEN_W - 4, cardH, 3, Theme::BORDER);

    y = cardY + 3;
    canvas.setTextColor(Theme::SECONDARY);
    canvas.setCursor(pad + 2, y);
    if (_radio && _radio->isRadioOnline()) {
        if (_userConfig) {
            const char* preset = detectPresetName(_userConfig->settings());
            canvas.printf("LoRa: %s  SF%d %luk",
                preset,
                _radio->getSpreadingFactor(),
                (unsigned long)(_radio->getSignalBandwidth() / 1000));
        }
    } else {
        canvas.setTextColor(Theme::MUTED);
        canvas.print("LoRa: OFFLINE");
    }
    y += lineH;

    if (_radio && _radio->isRadioOnline()) {
        canvas.setTextColor(Theme::MUTED);
        canvas.setCursor(pad + 2, y);
        canvas.printf("%.1f MHz  TX:%d dBm  CR:%d  P:%d L:%d",
            _radio->getFrequency() / 1000000.0,
            _radio->getTxPower(), _radio->getCodingRate4(),
            _rns ? (int)_rns->pathCount() : 0,
            _rns ? (int)_rns->linkCount() : 0);
    }
    y += lineH + 4;

    // === Announce button — vertically centered in remaining space ===
    {
        const char* label = "Announce [Enter]";
        int btnW = strlen(label) * Theme::CHAR_W + 16;
        int btnX = (Theme::SCREEN_W - btnW) / 2;
        int btnH = Theme::CHAR_H + 6;
        int remainingH = (Theme::CONTENT_Y + Theme::CONTENT_H) - y;
        int btnY = y + (remainingH - btnH) / 2;
        canvas.drawRoundRect(btnX, btnY, btnW, btnH, 3, Theme::PRIMARY);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(btnX + 8, btnY + 3);
        canvas.print(label);
    }

    // Announce flash toast
    if (millis() < _announceFlashUntil) {
        const char* msg = "Announced!";
        int tw = strlen(msg) * Theme::CHAR_W + 12;
        int th = Theme::CHAR_H + 8;
        int tx = (Theme::CONTENT_W - tw) / 2;
        int ty = Theme::CONTENT_Y + Theme::CONTENT_H - th - 4;
        canvas.fillRoundRect(tx, ty, tw, th, 3, Theme::SELECTION_BG);
        canvas.drawRoundRect(tx, ty, tw, th, 3, Theme::PRIMARY);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(tx + 6, ty + 4);
        canvas.print(msg);
    }
}
