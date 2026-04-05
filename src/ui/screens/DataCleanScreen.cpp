#include "DataCleanScreen.h"
#include "ui/Theme.h"
#include "config/Config.h"

void DataCleanScreen::render(M5Canvas& canvas) {
    int cx = Theme::SCREEN_W / 2;

    // "RATSPEAK" title
    canvas.setTextSize(2);
    canvas.setTextColor(Theme::PRIMARY);
    const char* titleStr = "RATSPEAK";
    int tw = strlen(titleStr) * 12;
    canvas.setCursor(cx - tw / 2, Theme::CONTENT_Y + 4);
    canvas.print(titleStr);

    // "ratspeak.org" subtitle
    canvas.setTextSize(1);
    canvas.setTextColor(Theme::MUTED);
    const char* sub = "ratspeak.org";
    int sw = strlen(sub) * Theme::CHAR_W;
    canvas.setCursor(cx - sw / 2, Theme::CONTENT_Y + 24);
    canvas.print(sub);

    // Message
    canvas.setTextColor(Theme::SECONDARY);
    const char* msg = "Old data found on SD card.";
    int mw = strlen(msg) * Theme::CHAR_W;
    canvas.setCursor(cx - mw / 2, Theme::CONTENT_Y + 40);
    canvas.print(msg);

    // Prompt
    const char* prompt = "Remove old data & start fresh?";
    int pw = strlen(prompt) * Theme::CHAR_W;
    canvas.setCursor(cx - pw / 2, Theme::CONTENT_Y + 52);
    canvas.print(prompt);

    if (_status) {
        // Show status message instead of buttons
        canvas.setTextColor(Theme::PRIMARY);
        int stw = strlen(_status) * Theme::CHAR_W;
        canvas.setCursor(cx - stw / 2, Theme::CONTENT_Y + 70);
        canvas.print(_status);
    } else {
        // Yes / No selection
        const char* yes = "[ Yes ]";
        const char* no  = "[ No ]";

        canvas.setTextColor(_selectedYes ? Theme::PRIMARY : Theme::MUTED);
        int yw = strlen(yes) * Theme::CHAR_W;
        canvas.setCursor(cx - 36 - yw / 2, Theme::CONTENT_Y + 70);
        canvas.print(yes);

        canvas.setTextColor(!_selectedYes ? Theme::PRIMARY : Theme::MUTED);
        int nw = strlen(no) * Theme::CHAR_W;
        canvas.setCursor(cx + 36 - nw / 2, Theme::CONTENT_Y + 70);
        canvas.print(no);

        // Hint
        canvas.setTextColor(Theme::MUTED);
        const char* hint = ";/.=Select  Enter=OK";
        int hw = strlen(hint) * Theme::CHAR_W;
        canvas.setCursor(cx - hw / 2, Theme::CONTENT_Y + 86);
        canvas.print(hint);
    }

    // Version
    canvas.setTextColor(Theme::BORDER);
    const char* ver = "v" RATCOM_VERSION_STRING;
    int vw = strlen(ver) * Theme::CHAR_W;
    canvas.setCursor(cx - vw / 2, Theme::CONTENT_Y + Theme::CONTENT_H - 12);
    canvas.print(ver);
}

bool DataCleanScreen::handleKey(const KeyEvent& event) {
    if (_status) return true;  // Locked during wipe

    // ; = left/up, . = right/down (same as settings nav)
    if (event.character == ';') {
        _selectedYes = true;
        return true;
    }
    if (event.character == '.') {
        _selectedYes = false;
        return true;
    }
    if (event.enter) {
        if (_doneCb) _doneCb(_selectedYes);
        return true;
    }
    return true;  // Consume all keys
}

void DataCleanScreen::setStatus(const char* msg) {
    _status = msg;
}
