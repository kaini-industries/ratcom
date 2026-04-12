#include "LXMFManager.h"
#include "config/Config.h"
#include <Transport.h>
#include <time.h>

LXMFManager* LXMFManager::_instance = nullptr;

bool LXMFManager::begin(ReticulumManager* rns, MessageStore* store) {
    _rns = rns;
    _store = store;
    _instance = this;

    // Register callbacks on the LXMF delivery destination
    RNS::Destination& dest = _rns->destination();
    dest.set_packet_callback(onPacketReceived);
    dest.set_link_established_callback(onLinkEstablished);

    // Pre-compute unread counts at boot — NEVER do this lazily in callbacks
    computeUnreadFromDisk();

    Serial.println("[LXMF] Manager started");
    return true;
}

void LXMFManager::loop() {
    // Process incoming messages queued by packet callbacks (safe context — main loop)
    while (!_incomingQueue.empty()) {
        LXMFMessage msg = std::move(_incomingQueue.front());
        _incomingQueue.erase(_incomingQueue.begin());

        Serial.printf("[LXMF] Processing: from %s \"%s\"\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str(),
                      msg.content.c_str());

        // Store to disk (takes mutex — safe here in main loop)
        if (_store) {
            _store->saveMessage(msg);
        }

        // Track unread (normalize to 16-char hex — matches conversation dir names)
        std::string peerHex = msg.sourceHash.toHex().substr(0, 16);
        _unread[peerHex]++;

        // Notify UI
        if (_onMessage) {
            _onMessage(msg);
        }
    }

    // Periodic path refresh for peers with queued messages (every 60s)
    {
        static unsigned long lastPathRefresh = 0;
        unsigned long now = millis();
        if (now - lastPathRefresh >= 60000) {
            lastPathRefresh = now;
            for (const auto& msg : _outQueue) {
                if (!RNS::Transport::has_path(msg.destHash)) {
                    RNS::Transport::request_path(msg.destHash);
                }
            }
        }
    }

    // Proactive stale link cleanup (every 10s)
    {
        static unsigned long lastLinkCheck = 0;
        if (millis() - lastLinkCheck >= 10000) {
            lastLinkCheck = millis();
            if (_outLink && _outLink.status() == RNS::Type::Link::CLOSED) {
                Serial.println("[LXMF] Cleaning stale outbound link (CLOSED)");
                _outLink = {RNS::Type::NONE};
                _outLinkPending = false;
            }
        }
    }

    if (_outQueue.empty()) return;
    unsigned long now = millis();
    int processed = 0;

    // Priority: process new messages (retries=0) before old retries.
    // Find the best candidate: lowest retry count that's past cooldown.
    // Exponential backoff: 2s, 4s, 8s, 16s, 30s cap — accounts for LoRa RTT.
    auto findBest = [&]() -> std::deque<LXMFMessage>::iterator {
        auto best = _outQueue.end();
        for (auto it = _outQueue.begin(); it != _outQueue.end(); ++it) {
            if (it->retries > 0) {
                int shift = std::min(it->retries, 4);  // cap at 2^4 = 16x
                unsigned long cooldown = std::min(30000UL, 2000UL * (1UL << shift));
                if ((millis() - it->lastRetryMs) < cooldown) continue;
            }
            if (best == _outQueue.end() || it->retries < best->retries) {
                best = it;
            }
        }
        return best;
    };

    while (processed < 3 && (processed == 0 || millis() - now < 10)) {
        auto it = findBest();
        if (it == _outQueue.end()) break;

        LXMFMessage& msg = *it;
        msg.lastRetryMs = millis();

        if (sendDirect(msg)) {
            processed++;
            Serial.printf("[LXMF] Queue drain: status=%s dest=%s\n",
                          msg.statusStr(), msg.destHash.toHex().substr(0, 8).c_str());
            if (_statusCb) {
                std::string peerHex = msg.destHash.toHex();
                _statusCb(peerHex, msg.timestamp, msg.status);
            }
            _outQueue.erase(it);
        } else {
            break;  // cooldown was just set — findBest will skip this one next time
        }
    }
}

bool LXMFManager::sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title) {
    LXMFMessage msg;
    msg.sourceHash = _rns->destination().hash();
    msg.destHash = destHash;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        msg.timestamp = (double)now;
    } else {
        msg.timestamp = millis() / 1000.0;
    }
    msg.content = content;
    msg.title = title;
    msg.incoming = false;
    msg.status = LXMFStatus::QUEUED;

    if ((int)_outQueue.size() >= RATCOM_MAX_OUTQUEUE) {
        Serial.printf("[LXMF] WARNING: Outgoing queue full (%d), dropping oldest\n",
                      (int)_outQueue.size());
        LXMFMessage& dropped = _outQueue.front();
        dropped.status = LXMFStatus::FAILED;
        if (_statusCb) {
            std::string peerHex = dropped.destHash.toHex();
            _statusCb(peerHex, dropped.timestamp, dropped.status);
        }
        _outQueue.pop_front();
    }

    _outQueue.push_back(msg);

    // Save outgoing message to disk immediately (creates conversation entry)
    if (_store) {
        _store->saveMessage(msg);
    }

    // Proactively request path so it's ready when sendDirect runs
    if (!RNS::Transport::has_path(destHash)) {
        RNS::Transport::request_path(destHash);
        Serial.printf("[LXMF] Message queued for %s (%d bytes) — requesting path\n",
                      destHash.toHex().substr(0, 8).c_str(), (int)content.size());
    } else {
        Serial.printf("[LXMF] Message queued for %s (%d bytes) — path known\n",
                      destHash.toHex().substr(0, 8).c_str(), (int)content.size());
    }
    return true;
}

bool LXMFManager::sendDirect(LXMFMessage& msg) {
    Serial.printf("[LXMF] sendDirect: dest=%s link=%s pending=%s\n",
        msg.destHash.toHex().substr(0, 12).c_str(),
        _outLink ? (_outLink.status() == RNS::Type::Link::ACTIVE ? "ACTIVE" : "INACTIVE") : "NONE",
        _outLinkPending ? "yes" : "no");

    // Reset stale link-pending state
    if (_outLinkPending) {
        bool linkReady = _outLink && _outLink.status() == RNS::Type::Link::ACTIVE;
        if (!linkReady) {
            _outLinkPending = false;
            _outLink = {RNS::Type::NONE};
            Serial.println("[LXMF] Clearing stale link-pending (link not active)");
        }
    }

    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) {
        msg.retries++;
        // Proactively request path on first retry and every 10 retries
        if (msg.retries == 1 || msg.retries % 10 == 0) {
            RNS::Transport::request_path(msg.destHash);
            Serial.printf("[LXMF] Requested path for %s\n",
                          msg.destHash.toHex().substr(0, 8).c_str());
        }
        if (msg.retries >= 30) {
            Serial.printf("[LXMF] recall FAILED for %s after %d retries\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        Serial.printf("[LXMF] recall pending for %s (retry %d/30)\n",
                      msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
        return false;
    }

    Serial.printf("[LXMF] recall OK: identity for %s\n",
                  msg.destHash.toHex().substr(0, 8).c_str());

    // Ensure path exists — without a path, Transport::outbound() broadcasts as
    // Header1 which the Python hub silently drops
    if (!RNS::Transport::has_path(msg.destHash)) {
        msg.retries++;
        if (msg.retries == 1 || msg.retries % 5 == 0) {
            Serial.printf("[LXMF] No path for %s, requesting (retry %d)\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            RNS::Transport::request_path(msg.destHash);
        }
        if (msg.retries >= 30) {
            Serial.printf("[LXMF] No path for %s after %d retries — FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        return false;  // keep in queue, retry later
    }

    Serial.printf("[LXMF] path OK: %s hops=%d\n",
                  msg.destHash.toHex().substr(0, 8).c_str(),
                  RNS::Transport::hops_to(msg.destHash));

    RNS::Destination outDest(
        recipientId,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );

    // packFull returns opportunistic format: [src:16][sig:64][msgpack]
    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) {
        Serial.println("[LXMF] Failed to pack message");
        msg.status = LXMFStatus::FAILED;
        return true;
    }

    msg.status = LXMFStatus::SENDING;
    bool sent = false;

    std::string peerHex = msg.destHash.toHex();

    // Try link-based delivery if we have an active link to this peer
    if (_outLink && _outLinkDestHash == msg.destHash
        && _outLink.status() == RNS::Type::Link::ACTIVE) {
        // Link delivery: prepend dest_hash (Python DIRECT format)
        std::vector<uint8_t> linkPayload;
        linkPayload.reserve(16 + payload.size());
        linkPayload.insert(linkPayload.end(), msg.destHash.data(), msg.destHash.data() + 16);
        linkPayload.insert(linkPayload.end(), payload.begin(), payload.end());
        RNS::Bytes linkBytes(linkPayload.data(), linkPayload.size());
        if (linkBytes.size() <= RNS::Type::Reticulum::MDU) {
            // Small enough for single link packet
            Serial.printf("[LXMF] sending via link packet: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Packet packet(_outLink, linkBytes);
            RNS::PacketReceipt receipt = packet.send();
            if (receipt) {
                sent = true;
                trackReceipt(receipt, peerHex, msg.timestamp);
            }
        } else {
            // Too large for single packet — use Resource transfer
            Serial.printf("[LXMF] sending via link resource: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            if (_outLink.start_resource_transfer(linkBytes)) {
                sent = true;
            } else {
                Serial.println("[LXMF] resource transfer failed to start");
            }
        }
    }

    // Fallback: opportunistic or queue for link-based resource transfer
    if (!sent) {
        RNS::Bytes payloadBytes(payload.data(), payload.size());
        if (payloadBytes.size() <= RNS::Type::Reticulum::MDU) {
            // Small enough for single opportunistic packet
            Serial.printf("[LXMF] sending opportunistic: %d bytes to %s\n",
                          (int)payloadBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Packet packet(outDest, payloadBytes);
            RNS::PacketReceipt receipt = packet.send();
            if (receipt) {
                sent = true;
                trackReceipt(receipt, peerHex, msg.timestamp);
            }
        } else {
            // Too large for opportunistic — need link + resource transfer
            Serial.printf("[LXMF] Message too large for opportunistic (%d bytes > MDU), needs link (retry %d)\n",
                          (int)payloadBytes.size(), msg.retries);
            if (msg.retries % 3 == 0 && (!_outLink || _outLinkDestHash != msg.destHash
                || _outLink.status() != RNS::Type::Link::ACTIVE)) {
                _outLinkPendingHash = msg.destHash;
                _outLinkPending = true;
                Serial.printf("[LXMF] Establishing link to %s for resource transfer\n",
                              msg.destHash.toHex().substr(0, 8).c_str());
                RNS::Link newLink(outDest, onOutLinkEstablished, onOutLinkClosed);
            }
            msg.retries++;
            if (msg.retries >= 30) {
                Serial.printf("[LXMF] Link for %s not established after %d retries — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
            return false;  // Keep in queue, retry when link is established
        }
    }

    if (sent) {
        msg.status = LXMFStatus::SENT;
        Serial.printf("[LXMF] SENT OK: msgId=%s\n", msg.messageId.toHex().substr(0, 8).c_str());
    } else {
        msg.status = LXMFStatus::FAILED;
        Serial.printf("[LXMF] Send FAILED to %s\n",
                      msg.destHash.toHex().substr(0, 8).c_str());
    }

    // Link establishment is now only triggered on-demand when a message
    // is too large for opportunistic delivery (see large-message path above).

    return true;
}

void LXMFManager::onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet) {
    if (!_instance) return;
    // Non-link delivery: dest_hash is NOT in LXMF payload (it's in the RNS packet header).
    // Reconstruct full format by prepending it, matching Python LXMRouter.delivery_packet().
    const RNS::Bytes& destHash = packet.destination_hash();
    std::vector<uint8_t> fullData;
    fullData.reserve(destHash.size() + data.size());
    fullData.insert(fullData.end(), destHash.data(), destHash.data() + destHash.size());
    fullData.insert(fullData.end(), data.data(), data.data() + data.size());
    _instance->processIncoming(fullData.data(), fullData.size(), destHash);
}

void LXMFManager::onOutLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    _instance->_outLink = link;
    _instance->_outLinkDestHash = _instance->_outLinkPendingHash;
    _instance->_outLinkPending = false;
    Serial.printf("[LXMF] Outbound link established to %s\n",
                  _instance->_outLinkDestHash.toHex().substr(0, 8).c_str());
}

void LXMFManager::onOutLinkClosed(RNS::Link& link) {
    if (!_instance) return;
    _instance->_outLink = {RNS::Type::NONE};
    _instance->_outLinkPending = false;
    Serial.println("[LXMF] Outbound link closed");
}

void LXMFManager::onLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    Serial.printf("[LXMF-DIAG] onLinkEstablished fired! link_id=%s status=%d\n",
        link.link_id().toHex().substr(0, 16).c_str(), (int)link.status());
    link.set_packet_callback([](const RNS::Bytes& data, const RNS::Packet& packet) {
        if (!_instance) return;
        Serial.printf("[LXMF-DIAG] Link packet received! %d bytes pkt_dest=%s\n",
            (int)data.size(), packet.destination_hash().toHex().substr(0, 16).c_str());
        // Link delivery: data already contains [dest:16][src:16][sig:64][msgpack]
        // Do NOT use packet.destination_hash() — that's the link_id, not the LXMF dest.
        _instance->processIncoming(data.data(), data.size(), RNS::Bytes());
    });
}

void LXMFManager::processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash) {
    // This runs in the packet callback context — MUST be non-blocking.
    // Only unpack and dedup here, defer storage/UI to loop().

    LXMFMessage msg;
    if (!LXMFMessage::unpackFull(data, len, msg)) {
        Serial.println("[LXMF] Failed to unpack message");
        return;
    }

    // Drop self-messages (loopback from Transport)
    if (_rns && msg.sourceHash == _rns->destination().hash()) {
        return;
    }

    // Deduplication
    std::string msgIdHex = msg.messageId.toHex();
    if (_seenMessageIds.count(msgIdHex)) {
        return;
    }
    _seenMessageIds.insert(msgIdHex);
    if ((int)_seenMessageIds.size() > MAX_SEEN_IDS) {
        _seenMessageIds.erase(_seenMessageIds.begin());
    }

    if (destHash.size() > 0) {
        msg.destHash = destHash;
    }

    // Use local receive time for incoming messages so all timestamps in
    // the conversation reflect THIS device's clock, not the sender's.
    // Prevents confusing display when a peer's clock is wrong/unsynced.
    {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            msg.timestamp = (double)now;
        }
    }

    // Queue for processing in loop() — no I/O, no mutex, no callbacks here
    if ((int)_incomingQueue.size() < MAX_INCOMING_QUEUE) {
        _incomingQueue.push_back(msg);
        Serial.printf("[LXMF] Queued incoming from %s\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str());
    } else {
        Serial.println("[LXMF] Incoming queue full, dropping message");
    }
}

const std::vector<std::string>& LXMFManager::conversations() const {
    if (_store) return _store->conversations();
    static std::vector<std::string> empty;
    return empty;
}

std::vector<LXMFMessage> LXMFManager::getMessages(const std::string& peerHex, int limit) const {
    if (_store) return _store->loadConversation(peerHex, limit);
    return {};
}

int LXMFManager::unreadCount(const std::string& peerHex) const {
    // Never do lazy disk I/O here — this is called from packet callbacks.
    // Unread counts are pre-computed at boot and updated incrementally.
    if (peerHex.empty()) {
        int total = 0;
        for (auto& kv : _unread) total += kv.second;
        return total;
    }
    // Normalize to 16-char hex — matches conversation dir names
    std::string key = peerHex.substr(0, 16);
    auto it = _unread.find(key);
    return (it != _unread.end()) ? it->second : 0;
}

void LXMFManager::computeUnreadFromDisk() {
    _unreadComputed = true;
    if (!_store) return;

    // Use efficient file-based unread counting (no JSON parsing needed)
    for (auto& conv : _store->conversations()) {
        int count = _store->unreadCountForPeer(conv);
        if (count > 0) _unread[conv] = count;
    }

    int totalUnread = 0;
    for (auto& kv : _unread) totalUnread += kv.second;
    if (totalUnread > 0) {
        Serial.printf("[LXMF] Restored %d unread messages\n", totalUnread);
    }
}

void LXMFManager::markRead(const std::string& peerHex) {
    // Normalize to 16-char hex — matches conversation dir names
    std::string key = peerHex.substr(0, 16);
    _unread[key] = 0;
    if (_store) {
        _store->markConversationRead(key);
    }
}

void LXMFManager::deleteConversation(const std::string& peerHex) {
    // Normalize to 16-char hex — matches conversation dir names
    std::string key = peerHex.substr(0, 16);
    if (_store) {
        _store->deleteConversation(key);
    }
    _unread.erase(key);
    Serial.printf("[LXMF] Conversation deleted: %s\n", key.substr(0, 8).c_str());
}

void LXMFManager::refreshPathForPeer(const std::string& peerHex) {
    if (peerHex.empty()) return;
    RNS::Bytes destHash;
    destHash.assignHex(peerHex.c_str());
    if (!RNS::Transport::has_path(destHash)) {
        RNS::Transport::request_path(destHash);
        Serial.printf("[LXMF] Path refresh requested for active peer %s\n",
                      peerHex.substr(0, 8).c_str());
    }
}

// =============================================================================
// Receipt tracking — delivery proof and timeout callbacks
// =============================================================================

void LXMFManager::trackReceipt(RNS::PacketReceipt receipt, const std::string& peerHex, double timestamp) {
    if (!receipt) return;

    // Register callbacks
    receipt.set_delivery_callback(onDeliveryProof);
    receipt.set_timeout_callback(onDeliveryTimeout);

    // Store mapping from receipt hash to message info
    std::string hashHex = receipt.hash().toHex();
    _pendingReceipts[hashHex] = {peerHex, timestamp};

    // Cap the map to prevent unbounded growth
    while ((int)_pendingReceipts.size() > MAX_PENDING_RECEIPTS) {
        _pendingReceipts.erase(_pendingReceipts.begin());
    }

    Serial.printf("[LXMF] Tracking receipt %s for %s\n",
                  hashHex.substr(0, 8).c_str(), peerHex.substr(0, 8).c_str());
}

void LXMFManager::onDeliveryProof(const RNS::PacketReceipt& receipt) {
    if (!_instance) return;

    std::string hashHex = receipt.hash().toHex();
    auto it = _instance->_pendingReceipts.find(hashHex);
    if (it != _instance->_pendingReceipts.end()) {
        Serial.printf("[LXMF] DELIVERED: proof received for %s\n",
                      it->second.peerHex.substr(0, 8).c_str());

        if (_instance->_statusCb) {
            _instance->_statusCb(it->second.peerHex, it->second.timestamp, LXMFStatus::DELIVERED);
        }

        _instance->_pendingReceipts.erase(it);
    } else {
        Serial.printf("[LXMF] Delivery proof for unknown receipt %s\n", hashHex.substr(0, 8).c_str());
    }
}

void LXMFManager::onDeliveryTimeout(const RNS::PacketReceipt& receipt) {
    if (!_instance) return;

    std::string hashHex = receipt.hash().toHex();
    auto it = _instance->_pendingReceipts.find(hashHex);
    if (it != _instance->_pendingReceipts.end()) {
        Serial.printf("[LXMF] Delivery timeout for %s\n",
                      it->second.peerHex.substr(0, 8).c_str());
        // Don't mark as FAILED — the packet was sent, just not proven.
        // Leave as SENT so user knows it went out.
        _instance->_pendingReceipts.erase(it);
    }
}
