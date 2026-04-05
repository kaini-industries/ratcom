#include "MessagesScreen.h"
#include "ui/Theme.h"
#include "reticulum/AnnounceManager.h"

void MessagesScreen::onEnter() {
    _showingContext = false;
    _enterHeld = false;
    _enterPressTime = 0;
    refreshList();
}

void MessagesScreen::refreshList() {
    if (!_lxmf) return;

    _list.clear();
    _peerHexes.clear();

    const auto& convs = _lxmf->conversations();
    for (const auto& peerHex : convs) {
        std::string label;
        if (_am) {
            const DiscoveredNode* node = _am->findNodeByHex(peerHex);
            if (node && !node->name.empty()) {
                label = node->name;
            }
        }
        if (label.empty()) {
            if (peerHex.size() >= 8) {
                label = peerHex.substr(0, 4) + ":" + peerHex.substr(4, 4);
            } else {
                label = peerHex;
            }
        }

        int unread = _lxmf->unreadCount(peerHex);
        if (unread > 0) {
            label += " [" + std::to_string(unread) + "]";
        }

        _list.addItem(label);
        _peerHexes.push_back(peerHex);
    }

    if (_list.itemCount() == 0) {
        _list.addItem("No conversations yet");
    }

    _lastRefresh = millis();
    _needsRefresh = false;
}

void MessagesScreen::showContextMenu(int idx) {
    if (idx < 0 || idx >= (int)_peerHexes.size()) return;
    _contextPeerHex = _peerHexes[idx];

    // Check if this peer is a saved contact
    _contextIsContact = false;
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_contextPeerHex);
        if (node && node->saved) _contextIsContact = true;
    }

    _contextList.clear();
    if (!_contextIsContact) {
        _contextList.addItem("Add Contact");
    }
    _contextList.addItem("Delete History");
    _contextList.addItem("Back");
    _contextList.setSelected(0);
    _showingContext = true;
}

void MessagesScreen::executeContextAction() {
    const std::string& action = _contextList.getSelectedItem();

    if (action == "Add Contact") {
        if (_addContactCb) _addContactCb(_contextPeerHex);
        exitContextMenu();
    } else if (action == "Delete History") {
        if (_lxmf) {
            _lxmf->deleteConversation(_contextPeerHex);
        }
        exitContextMenu();
        refreshList();
    } else {
        exitContextMenu();
    }
}

void MessagesScreen::exitContextMenu() {
    _showingContext = false;
    _contextPeerHex.clear();
}

void MessagesScreen::render(M5Canvas& canvas) {
    if (_needsRefresh) {
        refreshList();
    }

    int y = Theme::CONTENT_Y;

    // Header
    canvas.fillRect(0, y, Theme::CONTENT_W, 11, Theme::BAR_BG);
    canvas.fillRect(0, y, 2, 11, Theme::ACCENT);
    canvas.setTextColor(Theme::ACCENT);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.drawString("MESSAGES", 6, y + 2);
    canvas.drawFastHLine(0, y + 11, Theme::CONTENT_W, Theme::BORDER);
    y += 13;

    if (_showingContext) {
        // Context menu header
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(4, y);

        // Show peer name
        std::string label;
        if (_am) {
            const DiscoveredNode* node = _am->findNodeByHex(_contextPeerHex);
            if (node && !node->name.empty()) label = node->name;
        }
        if (label.empty()) label = _contextPeerHex.substr(0, 8);
        canvas.print(label.c_str());
        y += Theme::CHAR_H + 4;
        canvas.drawFastHLine(0, y, Theme::SCREEN_W, Theme::BORDER);
        y += 2;

        _contextList.render(canvas, 0, y, Theme::CONTENT_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
    } else {
        // Long-press hint
        if (_enterHeld && millis() - _enterPressTime >= LONG_PRESS_MS) {
            // Show context menu on long press
            int idx = _list.getSelectedIndex();
            showContextMenu(idx);
        }

        _list.render(canvas, 0, y, Theme::CONTENT_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
    }
}

bool MessagesScreen::handleKey(const KeyEvent& event) {
    if (_showingContext) {
        if (event.character == 27 || event.del) {
            exitContextMenu();
            return true;
        }
        if (event.character == ';') { _contextList.scrollUp(); return true; }
        if (event.character == '.') { _contextList.scrollDown(); return true; }
        if (event.enter) {
            executeContextAction();
            return true;
        }
        return true;
    }

    if (event.character == ';') { _list.scrollUp(); return true; }
    if (event.character == '.') { _list.scrollDown(); return true; }

    // Enter key: track press time for long-press detection
    if (event.enter) {
        if (_enterPressTime == 0) {
            _enterPressTime = millis();
            _enterHeld = true;
        } else if (millis() - _enterPressTime >= LONG_PRESS_MS) {
            // Long press — show context menu
            _enterHeld = false;
            _enterPressTime = 0;
            int idx = _list.getSelectedIndex();
            showContextMenu(idx);
        } else {
            // Quick press — open conversation
            _enterHeld = false;
            _enterPressTime = 0;
            int idx = _list.getSelectedIndex();
            if (idx >= 0 && idx < (int)_peerHexes.size() && _openCb) {
                _openCb(_peerHexes[idx]);
            }
        }
        return true;
    }

    // Reset long-press tracking on any other key
    _enterHeld = false;
    _enterPressTime = 0;

    return false;
}
