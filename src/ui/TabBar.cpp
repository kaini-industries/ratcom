#include "TabBar.h"
#include "Theme.h"

constexpr const char* TabBar::TAB_LABELS[4];

void TabBar::setActiveTab(int tab) {
    if (tab >= 0 && tab < Theme::TAB_COUNT) {
        _activeTab = tab;
        if (_dirty) *_dirty = true;
    }
}

void TabBar::cycleTab(int direction) {
    _activeTab = (_activeTab + direction + Theme::TAB_COUNT) % Theme::TAB_COUNT;
    if (_dirty) *_dirty = true;
}

void TabBar::setUnreadCount(int tab, int count) {
    if (tab >= 0 && tab < Theme::TAB_COUNT) {
        _unreadCounts[tab] = count;
        if (_dirty) *_dirty = true;
    }
}

void TabBar::render(M5Canvas& canvas) {
    int y = Theme::SCREEN_H - Theme::TAB_BAR_H;

    // Dark panel background
    canvas.fillRect(0, y, Theme::SCREEN_W, Theme::TAB_BAR_H, Theme::BAR_BG);
    canvas.drawFastHLine(0, y, Theme::SCREEN_W, Theme::BORDER);

    canvas.setTextSize(Theme::FONT_SIZE);

    for (int i = 0; i < Theme::TAB_COUNT; i++) {
        int tx = i * Theme::TAB_W;
        bool active = (i == _activeTab);

        int labelLen = strlen(TAB_LABELS[i]) * Theme::CHAR_W;
        int labelX = tx + (Theme::TAB_W - labelLen) / 2;
        int labelY = y + (Theme::TAB_BAR_H - Theme::CHAR_H) / 2;

        // Active tab: filled rounded pill behind text
        if (active) {
            int pillW = labelLen + 8;
            int pillX = labelX - 4;
            int pillY = labelY - 3;
            int pillH = Theme::CHAR_H + 5;
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 3, Theme::SELECTION_BG);
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 3, Theme::PRIMARY);
        }

        // Label color — blink Msgs tab when unread
        uint16_t labelColor;
        if (active) {
            labelColor = Theme::TAB_ACTIVE;
        } else if (i == TAB_MSGS && _unreadCounts[TAB_MSGS] > 0) {
            bool blinkOn = ((millis() / 1500) % 2) == 0;
            labelColor = blinkOn ? Theme::WARNING : Theme::TAB_INACTIVE;
        } else {
            labelColor = Theme::TAB_INACTIVE;
        }
        canvas.setTextColor(labelColor);
        canvas.setCursor(labelX, labelY);
        canvas.print(TAB_LABELS[i]);

        // Unread badge (small pill)
        if (_unreadCounts[i] > 0) {
            char badge[8];
            snprintf(badge, sizeof(badge), "%d", _unreadCounts[i]);
            int badgeW = strlen(badge) * Theme::CHAR_W + 6;
            int badgeX = labelX + labelLen + 2;
            int badgeY = labelY - 1;
            canvas.fillRoundRect(badgeX, badgeY, badgeW, Theme::CHAR_H + 2, 3, Theme::BADGE_BG);
            canvas.setTextColor(Theme::BADGE_TEXT);
            canvas.setCursor(badgeX + 3, badgeY + 1);
            canvas.print(badge);
        }
    }
}
