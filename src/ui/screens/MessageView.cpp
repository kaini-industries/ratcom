#include "MessageView.h"
#include "ui/Theme.h"
#include "reticulum/AnnounceManager.h"
#include <Transport.h>
#include <time.h>

void MessageView::onEnter() {
    _input.setActive(true);
    _input.clear();
    _input.setMaxLength(400);
    _input.setSubmitCallback([this](const std::string& text) {
        sendCurrentInput();
    });
    refreshMessages();
}

void MessageView::refreshMessages() {
    if (!_lxmf || _peerHex.empty()) return;

    // Load last 20 messages (paginated)
    _messages = _lxmf->getMessages(_peerHex);

    // Merge any pending messages not yet flushed to disk.
    // Only remove pending entries that were confirmed written (found in disk load).
    // This prevents messages from vanishing if the WriteQueue hasn't flushed yet.
    // Use normalized 16-char key to match how notifyNewMessage() stores pending messages
    std::string pendKey = _peerHex.substr(0, 16);
    auto pendIt = _pendingMessages.find(pendKey);
    if (pendIt != _pendingMessages.end()) {
        auto& pendings = pendIt->second;
        // _messages currently holds only disk-loaded messages (from getMessages above).
        // Check each pending against disk; add if missing, mark confirmed if found.
        const auto diskMessages = _messages;  // snapshot before merge
        for (const auto& pending : pendings) {
            bool onDisk = false;
            for (const auto& loaded : diskMessages) {
                if (loaded.timestamp == pending.timestamp &&
                    loaded.sourceHash == pending.sourceHash) {
                    onDisk = true; break;
                }
            }
            if (!onDisk) _messages.push_back(pending);
        }
        // Remove only confirmed (on-disk) pending entries; keep unwritten ones
        pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
            [&diskMessages](const LXMFMessage& pending) {
                for (const auto& loaded : diskMessages) {
                    if (loaded.timestamp == pending.timestamp &&
                        loaded.sourceHash == pending.sourceHash) return true;
                }
                return false;
            }), pendings.end());
        if (pendings.empty()) _pendingMessages.erase(pendIt);
    }

    _lxmf->markRead(_peerHex);
    if (_unreadCb) _unreadCb();

    // Build chat lines (IRC-style)
    _chatLines.clear();
    for (const auto& msg : _messages) {
        ChatLine line;

        // Smart timestamp: real HH:MM if epoch, relative if uptime (capped at 24h)
        char ts[16];
        double tsVal = msg.timestamp;
        if (tsVal > 1700000000) {
            time_t t = (time_t)tsVal;
            struct tm* tm = localtime(&t);
            snprintf(ts, sizeof(ts), "%02d:%02d", tm->tm_hour, tm->tm_min);
        } else if (tsVal > 0 && (millis() / 1000) > (unsigned long)tsVal) {
            unsigned long ago = (millis() / 1000) - (unsigned long)tsVal;
            if (ago < 60) snprintf(ts, sizeof(ts), "%lus", ago);
            else if (ago < 3600) snprintf(ts, sizeof(ts), "%lum", ago / 60);
            else if (ago < 86400) snprintf(ts, sizeof(ts), "%luh", ago / 3600);
            else snprintf(ts, sizeof(ts), "--:--");
        } else {
            snprintf(ts, sizeof(ts), "--:--");
        }

        if (msg.incoming) {
            std::string sender = peerDisplayName();
            line.text = std::string(ts) + " " + sender + "> " + msg.content;
            line.color = Theme::PRIMARY;
        } else {
            // Status indicator suffix for outgoing messages
            const char* statusTag = "";
            uint16_t statusColor = Theme::PRIMARY;
            switch (msg.status) {
                case LXMFStatus::QUEUED:
                case LXMFStatus::SENDING:
                    statusTag = " ~";
                    statusColor = Theme::WARNING;
                    break;
                case LXMFStatus::SENT:
                    statusTag = " >";
                    statusColor = Theme::MUTED;
                    break;
                case LXMFStatus::DELIVERED:
                    statusTag = " >>";
                    statusColor = Theme::ACCENT;
                    break;
                case LXMFStatus::FAILED:
                    statusTag = " X";
                    statusColor = Theme::ERROR;
                    break;
                default:
                    break;
            }
            line.text = std::string(ts) + " you> " + msg.content + statusTag;
            line.color = statusColor;
        }

        _chatLines.push_back(line);
    }

    // Auto-scroll to bottom
    int maxCharsPerLine = Theme::CONTENT_W / Theme::CHAR_W;
    int totalWrappedLines = 0;
    for (const auto& cl : _chatLines) {
        totalWrappedLines += ((int)cl.text.size() / maxCharsPerLine) + 1;
    }
    int visibleLines = (Theme::CONTENT_H - 24) / Theme::CHAR_H;
    _scrollOffset = std::max(0, totalWrappedLines - visibleLines);

    _lastRefresh = millis();
    _needsRefresh = false;
}

void MessageView::render(M5Canvas& canvas) {
    // Event-driven refresh instead of timer-based
    if (_needsRefresh) {
        refreshMessages();
    }

    int baseY = Theme::CONTENT_Y;

    // Header: node name or peer hash + path status + back hint
    std::string header;
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_peerHex);
        if (node && !node->name.empty()) header = node->name;
    }
    if (header.empty()) {
        if (_peerHex.size() >= 8) {
            header = _peerHex.substr(0, 4) + ":" + _peerHex.substr(4, 4);
        } else {
            header = _peerHex;
        }
    }
    canvas.setTextColor(Theme::PRIMARY);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.drawString(header.c_str(), 4, baseY);

    // Path status indicator (right of name, left of [Esc])
    {
        RNS::Bytes destHash;
        destHash.assignHex(_peerHex.c_str());
        bool hasPath = RNS::Transport::has_path(destHash);
        const char* pathStr;
        uint16_t pathColor;
        if (hasPath) {
            int hops = RNS::Transport::hops_to(destHash);
            static char pathBuf[16];
            snprintf(pathBuf, sizeof(pathBuf), "%dhop", hops);
            pathStr = pathBuf;
            pathColor = Theme::SECONDARY;
        } else {
            pathStr = "no path";
            pathColor = Theme::ERROR;
        }
        int nameW = header.size() * Theme::CHAR_W + 8;
        canvas.setTextColor(pathColor);
        canvas.drawString(pathStr, nameW, baseY);
    }

    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("[Esc]", Theme::CONTENT_W - 30, baseY);

    // Separator
    canvas.drawFastHLine(0, baseY + 9, Theme::CONTENT_W, Theme::BORDER);

    // Chat area
    int chatY = baseY + 11;
    int chatH = Theme::CONTENT_H - 24;
    int maxCharsPerLine = Theme::CONTENT_W / Theme::CHAR_W;
    int currentLine = 0;

    for (const auto& cl : _chatLines) {
        int lineLen = cl.text.size();
        int pos = 0;
        while (pos < lineLen) {
            int remaining = lineLen - pos;
            int chars = std::min(remaining, maxCharsPerLine);

            if (currentLine >= _scrollOffset) {
                int drawY = chatY + (currentLine - _scrollOffset) * Theme::CHAR_H;
                if (drawY + Theme::CHAR_H > chatY + chatH) break;

                canvas.setTextColor(cl.color);
                // Stack buffer avoids heap allocation per line per frame
                char segment[48];
                int segLen = std::min(chars, (int)sizeof(segment) - 1);
                memcpy(segment, cl.text.c_str() + pos, segLen);
                segment[segLen] = '\0';
                canvas.drawString(segment, 2, drawY);
            }

            pos += chars;
            currentLine++;
        }
    }

    // "New message below" indicator when scrolled up
    if (_hasNewBelow) {
        canvas.setTextColor(Theme::WARNING);
        canvas.drawString("[new msg]", Theme::CONTENT_W - 54, chatY + chatH - Theme::CHAR_H);
    }

    // Input separator
    int inputY = baseY + Theme::CONTENT_H - 12;
    canvas.drawFastHLine(0, inputY - 2, Theme::CONTENT_W, Theme::BORDER);

    // Text input
    _input.render(canvas, 0, inputY, Theme::CONTENT_W);
}

bool MessageView::handleKey(const KeyEvent& event) {
    // Escape = back to messages list
    if (event.character == 27) {
        if (_backCb) _backCb();
        return true;
    }

    // Fn+Delete = back (even when text input is active)
    if (event.fn && event.del) {
        if (_backCb) _backCb();
        return true;
    }

    // Delete key = back ONLY if text input is empty
    if (event.del && !event.fn && _input.getText().empty()) {
        if (_backCb) _backCb();
        return true;
    }

    // Pass to text input
    if (_input.handleKey(event)) {
        return true;
    }

    return false;
}

std::string MessageView::peerDisplayName() const {
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_peerHex);
        if (node && !node->name.empty()) return node->name;
    }
    return _peerHex.size() >= 4 ? _peerHex.substr(0, 4) : _peerHex;
}

void MessageView::notifyNewMessage(const LXMFMessage& msg) {
    std::string senderHexFull = msg.incoming ?
        msg.sourceHash.toHex() : msg.destHash.toHex();
    // Normalize to 16-char hex — matches conversation dir names and _peerHex from messages list
    std::string senderHex = senderHexFull.substr(0, 16);

    // Check if this message is for the peer we're currently viewing
    // Compare normalized 16-char prefixes to handle mixed 16/32-char _peerHex
    bool match = (_peerHex.substr(0, 16) == senderHex);

    // Always store in pending for merge on next refreshMessages()
    // This ensures messages survive the async write delay
    _pendingMessages[senderHex].push_back(msg);

    if (!match) {
        _needsRefresh = true;
        return;
    }

    // Add directly to chat lines for real-time display
    ChatLine line;
    char ts[16];
    double tsVal = msg.timestamp;
    if (tsVal > 1700000000) {
        time_t t = (time_t)tsVal;
        struct tm* tm = localtime(&t);
        snprintf(ts, sizeof(ts), "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else if (tsVal > 0 && (millis() / 1000) > (unsigned long)tsVal) {
        unsigned long ago = (millis() / 1000) - (unsigned long)tsVal;
        if (ago < 60) snprintf(ts, sizeof(ts), "%lus", ago);
        else if (ago < 3600) snprintf(ts, sizeof(ts), "%lum", ago / 60);
        else if (ago < 86400) snprintf(ts, sizeof(ts), "%luh", ago / 3600);
        else snprintf(ts, sizeof(ts), "--:--");
    } else {
        snprintf(ts, sizeof(ts), "--:--");
    }

    if (msg.incoming) {
        std::string sender = peerDisplayName();
        line.text = std::string(ts) + " " + sender + "> " + msg.content;
        line.color = Theme::PRIMARY;
    } else {
        const char* statusTag = "";
        uint16_t statusColor = Theme::PRIMARY;
        switch (msg.status) {
            case LXMFStatus::QUEUED:
            case LXMFStatus::SENDING:
                statusTag = " ~"; statusColor = Theme::WARNING; break;
            case LXMFStatus::SENT:
                statusTag = " >"; statusColor = Theme::MUTED; break;
            case LXMFStatus::DELIVERED:
                statusTag = " >>"; statusColor = Theme::ACCENT; break;
            case LXMFStatus::FAILED:
                statusTag = " X"; statusColor = Theme::ERROR; break;
            default: break;
        }
        line.text = std::string(ts) + " you> " + msg.content + statusTag;
        line.color = statusColor;
    }
    _chatLines.push_back(line);

    // Smart auto-scroll: only scroll if user is already at/near bottom
    int maxCharsPerLine = Theme::CONTENT_W / Theme::CHAR_W;
    int totalWrappedLines = 0;
    for (const auto& cl : _chatLines) {
        totalWrappedLines += ((int)cl.text.size() / maxCharsPerLine) + 1;
    }
    int visibleLines = (Theme::CONTENT_H - 24) / Theme::CHAR_H;
    int maxScroll = std::max(0, totalWrappedLines - visibleLines);
    if (_scrollOffset >= maxScroll - 2) {
        // User is at/near bottom — auto-scroll
        _scrollOffset = maxScroll;
        _hasNewBelow = false;
    } else {
        // User is reading history — don't scroll, show indicator
        _hasNewBelow = true;
    }

    if (msg.incoming && _lxmf) {
        _lxmf->markRead(_peerHex);
        if (_unreadCb) _unreadCb();
    }
}

void MessageView::notifyStatusChange(const std::string& peerHex, double timestamp, LXMFStatus status) {
    // Compare normalized 16-char prefixes to handle mixed 16/32-char strings
    if (peerHex.substr(0, 16) != _peerHex.substr(0, 16)) return;

    // Find the most recent outgoing chat line that isn't already in a terminal state
    for (int i = _chatLines.size() - 1; i >= 0; i--) {
        auto& line = _chatLines[i];
        if (line.text.find("you>") == std::string::npos) continue;
        // Skip lines already in terminal states (DELIVERED or FAILED)
        if (line.color == Theme::ACCENT || line.color == Theme::ERROR) continue;

        // Update status tag and color
        auto stripTag = [](std::string& text) {
            // Remove trailing status tags: " ~", " >", " >>", " X"
            for (const char* tag : {" >>", " >", " ~", " X"}) {
                size_t pos = text.rfind(tag);
                if (pos != std::string::npos && pos == text.size() - strlen(tag)) {
                    text.erase(pos);
                    return;
                }
            }
        };

        stripTag(line.text);
        switch (status) {
            case LXMFStatus::SENT:
                line.text += " >";
                line.color = Theme::MUTED;
                break;
            case LXMFStatus::DELIVERED:
                line.text += " >>";
                line.color = Theme::ACCENT;
                break;
            case LXMFStatus::FAILED:
                line.text += " X";
                line.color = Theme::ERROR;
                break;
            default:
                break;
        }
        break;
    }
}

void MessageView::sendCurrentInput() {
    if (!_lxmf || _peerHex.empty()) return;

    std::string text = _input.getText();
    if (text.empty()) return;

    RNS::Bytes destHash;
    destHash.assignHex(_peerHex.c_str());

    _lxmf->sendMessage(destHash, text);
    _input.clear();

    // Store outgoing message in pending so it survives refresh before disk write completes
    {
        LXMFMessage pending;
        pending.destHash = destHash;
        pending.sourceHash = _lxmf->getSourceHash();
        pending.content = text;
        pending.timestamp = (time(nullptr) > 1700000000) ? (double)time(nullptr) : millis() / 1000.0;
        pending.incoming = false;
        pending.status = LXMFStatus::QUEUED;
        _pendingMessages[_peerHex.substr(0, 16)].push_back(pending);
    }

    // Add sent message to display immediately
    char ts[16];
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm* tm = localtime(&now);
        snprintf(ts, sizeof(ts), "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        snprintf(ts, sizeof(ts), "0s");
    }

    ChatLine line;
    line.text = std::string(ts) + " you> " + text + " ~";
    line.color = Theme::WARNING;  // QUEUED — will update to SENT/DELIVERED via callback
    _chatLines.push_back(line);

    // Always scroll to bottom when sending
    int maxCharsPerLine = Theme::CONTENT_W / Theme::CHAR_W;
    int totalWrappedLines = 0;
    for (const auto& cl : _chatLines) {
        totalWrappedLines += ((int)cl.text.size() / maxCharsPerLine) + 1;
    }
    int visibleLines = (Theme::CONTENT_H - 24) / Theme::CHAR_H;
    _scrollOffset = std::max(0, totalWrappedLines - visibleLines);
    _hasNewBelow = false;
}
