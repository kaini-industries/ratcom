#include "ScrollList.h"

static const std::string EMPTY_ITEM;

void ScrollList::setItems(const std::vector<std::string>& items) {
    _items = items;
    _itemColors.assign(items.size(), 0);
    _selected = 0;
    _scrollOffset = 0;
}

void ScrollList::addItem(const std::string& item) {
    _items.push_back(item);
    _itemColors.push_back(0);
}

void ScrollList::addItem(const std::string& item, uint16_t color) {
    _items.push_back(item);
    _itemColors.push_back(color);
}

void ScrollList::clear() {
    _items.clear();
    _itemColors.clear();
    _selected = 0;
    _scrollOffset = 0;
}

void ScrollList::setSelected(int idx) {
    if (idx >= 0 && idx < (int)_items.size()) {
        _selected = idx;
        if (_visibleRows > 0) {
            if (_selected < _scrollOffset) _scrollOffset = _selected;
            else if (_selected >= _scrollOffset + _visibleRows)
                _scrollOffset = _selected - _visibleRows + 1;
        }
    }
}

void ScrollList::scrollUp() {
    if (_items.empty()) return;
    if (_selected > 0) {
        _selected--;
    } else {
        _selected = (int)_items.size() - 1;  // Wrap to bottom
    }
    if (_selected < _scrollOffset) {
        _scrollOffset = _selected;
    }
    if (_selected >= _scrollOffset + _visibleRows) {
        _scrollOffset = _selected - _visibleRows + 1;
    }
}

void ScrollList::scrollDown() {
    if (_items.empty()) return;
    if (_selected < (int)_items.size() - 1) {
        _selected++;
    } else {
        _selected = 0;  // Wrap to top
        _scrollOffset = 0;
    }
    if (_selected >= _scrollOffset + _visibleRows) {
        _scrollOffset = _selected - _visibleRows + 1;
    }
}

const std::string& ScrollList::getSelectedItem() const {
    if (_selected >= 0 && _selected < (int)_items.size()) {
        return _items[_selected];
    }
    return EMPTY_ITEM;
}

void ScrollList::render(M5Canvas& canvas, int x, int y, int w, int h) {
    int rowH = Theme::CHAR_H + 4;
    _visibleRows = h / rowH;

    canvas.setTextSize(Theme::FONT_SIZE);

    for (int i = 0; i < _visibleRows && (i + _scrollOffset) < (int)_items.size(); i++) {
        int idx = i + _scrollOffset;
        int ry = y + i * rowH;

        // Selection highlight — rounded pill
        if (idx == _selected) {
            canvas.fillRoundRect(x + 1, ry, w - 2, rowH, 3, Theme::SELECTION_BG);
            canvas.drawRoundRect(x + 1, ry, w - 2, rowH, 3, Theme::PRIMARY);
            canvas.setTextColor(Theme::PRIMARY);
        } else {
            // Use per-item color if set, otherwise default
            uint16_t itemColor = (idx < (int)_itemColors.size() && _itemColors[idx] != 0)
                                 ? _itemColors[idx] : Theme::SECONDARY;
            canvas.setTextColor(itemColor);
        }

        // Truncate text to fit width (stack buffer avoids heap allocation per frame)
        int maxChars = w / Theme::CHAR_W;
        canvas.setCursor(x + 2, ry + 2);
        if ((int)_items[idx].length() > maxChars) {
            char truncBuf[64];
            int copyLen = std::min(maxChars - 2, (int)sizeof(truncBuf) - 3);
            if (copyLen > (int)_items[idx].length()) copyLen = _items[idx].length();
            memcpy(truncBuf, _items[idx].c_str(), copyLen);
            truncBuf[copyLen] = '.';
            truncBuf[copyLen + 1] = '.';
            truncBuf[copyLen + 2] = '\0';
            canvas.print(truncBuf);
        } else {
            canvas.print(_items[idx].c_str());
        }
    }

    // Scroll indicator — rounded track
    if ((int)_items.size() > _visibleRows) {
        int barH = h * _visibleRows / _items.size();
        if (barH < 6) barH = 6;
        int barY = y + (_scrollOffset * h / _items.size());
        canvas.fillRoundRect(x + w - 3, barY, 3, barH, 1, Theme::MUTED);
    }
}
