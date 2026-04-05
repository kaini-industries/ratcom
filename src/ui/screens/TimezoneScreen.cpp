#include "TimezoneScreen.h"
#include "ui/Theme.h"

void TimezoneScreen::onEnter() {
    _list.clear();
    for (int i = 0; i < TIMEZONE_COUNT; i++) {
        _list.addItem(TIMEZONE_TABLE[i].label);
    }
    _list.setSelected(_selectedIdx);
}

void TimezoneScreen::render(M5Canvas& canvas) {
    int y = Theme::CONTENT_Y;

    // Header card
    canvas.fillRect(0, y, Theme::CONTENT_W, 11, Theme::BAR_BG);
    canvas.fillRect(0, y, 2, 11, Theme::ACCENT);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.setTextColor(Theme::ACCENT);
    canvas.drawString("SELECT TIMEZONE", 6, y + 2);
    canvas.drawFastHLine(0, y + 11, Theme::CONTENT_W, Theme::BORDER);
    y += 13;

    // Hint
    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("Use ;/. to scroll, Enter to select", 4, y);
    y += Theme::CHAR_H + 4;

    // List
    _list.render(canvas, 0, y, Theme::CONTENT_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
}

bool TimezoneScreen::handleKey(const KeyEvent& event) {
    if (event.character == ';') {
        _list.scrollUp();
        return true;
    }
    if (event.character == '.') {
        _list.scrollDown();
        return true;
    }
    if (event.enter) {
        int idx = _list.getSelectedIndex();
        if (idx >= 0 && idx < TIMEZONE_COUNT && _doneCb) {
            _doneCb(idx);
        }
        return true;
    }
    return false;
}
