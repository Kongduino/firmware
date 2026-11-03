#pragma once

#include "configuration.h"
#include <OLEDDisplay.h>
#include <functional>
#include <string>

namespace graphics
{

enum VirtualKeyType { VK_CHAR, VK_BACKSPACE, VK_ENTER, VK_SHIFT, VK_ESC, VK_SPACE };

struct VirtualKey {
    char character;
    VirtualKeyType type;
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
};

class VirtualKeyboard
{
  public:
    VirtualKeyboard();
    ~VirtualKeyboard();

    void draw(OLEDDisplay *display, int16_t offsetX, int16_t offsetY);
    void setInputText(const std::string &text);
    void setHeader(const std::string &header);
    void setCallback(std::function<void(const std::string &)> callback);

    // Navigation methods for encoder input
    void moveCursorUp();
    void moveCursorDown();
    void moveCursorLeft();
    void moveCursorRight();
    void handlePress();
    void handleLongPress();

    // Timeout management
    void resetTimeout();
    bool isTimedOut() const;

  private:
    static const uint8_t KEYBOARD_ROWS = 4;
    static const uint8_t KEYBOARD_COLS = 11;
    static const uint8_t KEY_WIDTH = 9;
    static const uint8_t KEY_HEIGHT = 9;        // Compressed to fit 4 rows on 64px displays
    static const uint8_t KEYBOARD_START_Y = 26; // Start just below input box bottom

    VirtualKey keyboard[KEYBOARD_ROWS][KEYBOARD_COLS];

    std::string inputText;
    std::string headerText;
    std::function<void(const std::string &)> onTextEntered;

    uint8_t cursorRow;
    uint8_t cursorCol;

    // Timeout management for auto-exit
    uint32_t lastActivityTime;
    static const uint32_t TIMEOUT_MS = 60000; // 1 minute timeout

    void initializeKeyboard();
    void drawKey(OLEDDisplay *display, const VirtualKey &key, bool selected, int16_t x, int16_t y, uint8_t w, uint8_t h,
                 bool isLastCol);
    void drawInputArea(OLEDDisplay *display, int16_t offsetX, int16_t offsetY, int16_t keyboardStartY);

    // Unified cursor movement helper
    void moveCursorDelta(int dRow, int dCol);

    char getCharForKey(const VirtualKey &key, bool isLongPress = false);
    void insertCharacter(char c);
    void deleteCharacter();
    void submitText();

    // Stroke input (五笔画), with a stroke dictionary and font packs installed: a second layout. In letter mode the ';'
    // key shows 筆 and switches to it (a long press still types ';'). As on a phone's stroke keypad, the strokes and the
    // wildcard make a 3 x 2 pad on the left, with the candidate pages and EN (back to letters) under it; three rows of
    // seven candidates beside it, and a row of CJK punctuation at the bottom
    bool strokesAvailable = false;
    bool strokeMode = false;
    int candidatePage = 0; // pages of kCandidatesPerPage

    bool handleStrokePress(bool isLongPress); // true when the stroke layout took the press
    void pickCandidate(int index);
    bool insertText(const std::string &text); // whole or not at all, within the length limit
    std::string keyLabel(int row, int col);   // a key's label in the current layout ("" for none)
    void drawLabeledKey(OLEDDisplay *display, const std::string &keyText, bool isAction, bool selected, int16_t x, int16_t y,
                        uint8_t width, uint8_t height, bool isLastCol);
    int textWidth(OLEDDisplay *display, const std::string &s); // through the font packs when there are any
    void drawText(OLEDDisplay *display, int x, int y, const std::string &s);
};

} // namespace graphics
