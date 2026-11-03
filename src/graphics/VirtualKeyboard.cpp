#include "configuration.h"
#if HAS_SCREEN
#include "VirtualKeyboard.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/fontpack/FontPacks.h" // defines HAS_FONTPACKS
#include "main.h"
#include "meshUtils.h"
#include <Arduino.h>
#include <vector>
#if HAS_FONTPACKS
#include "WaypointUtils.h" // utf8FromCodepoint
#include "graphics/EmoteRenderer.h"
#include "graphics/emotes.h"
#include "graphics/fontpack/StrokeInput.h"
#endif

namespace graphics
{

#if HAS_FONTPACKS
namespace
{
constexpr int kCandidatesPerPage = 20; // two rows of 10
// Labels the built-in font or a CJK pack has (‹ › … aren't in either)
const char *const kStrokeRow[] = {"一", "丨", "丿", "丶", "乙", "*", "<", ">", "EN"};
const char *const kPunctRow[] = {"，", "。", "？", "！", "、", "：", "；", "「", "」", "～"};
} // namespace
#endif

VirtualKeyboard::VirtualKeyboard() : cursorRow(0), cursorCol(0), lastActivityTime(millis())
{
    initializeKeyboard();
    // Set cursor to H(2, 5)
    cursorRow = 2;
    cursorCol = 5;
#if HAS_FONTPACKS
    strokesAvailable = StrokeInput::available();
#endif
}

VirtualKeyboard::~VirtualKeyboard() {}

void VirtualKeyboard::initializeKeyboard()
{
    // New 4 row, 11 column keyboard layout:
    static const char LAYOUT[KEYBOARD_ROWS][KEYBOARD_COLS] = {{'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\b'},
                                                              {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '\n'},
                                                              {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', ' '},
                                                              {'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', ',', '?', '\x1b'}};

    // Derive layout dimensions and assert they match the configured keyboard grid
    constexpr int LAYOUT_ROWS = (int)(sizeof(LAYOUT) / sizeof(LAYOUT[0]));
    constexpr int LAYOUT_COLS = (int)(sizeof(LAYOUT[0]) / sizeof(LAYOUT[0][0]));
    static_assert(LAYOUT_ROWS == KEYBOARD_ROWS, "LAYOUT rows must equal KEYBOARD_ROWS");
    static_assert(LAYOUT_COLS == KEYBOARD_COLS, "LAYOUT cols must equal KEYBOARD_COLS");

    // Initialize all keys to empty first
    for (int row = 0; row < LAYOUT_ROWS; row++) {
        for (int col = 0; col < LAYOUT_COLS; col++) {
            keyboard[row][col] = {0, VK_CHAR, 0, 0, 0, 0};
        }
    }

    // Fill keyboard from the 2D layout
    for (int row = 0; row < LAYOUT_ROWS; row++) {
        for (int col = 0; col < LAYOUT_COLS; col++) {
            char ch = LAYOUT[row][col];
            // No empty slots in the simplified layout

            VirtualKeyType type = VK_CHAR;
            if (ch == '\b') {
                type = VK_BACKSPACE;
            } else if (ch == '\n') {
                type = VK_ENTER;
            } else if (ch == '\x1b') { // ESC
                type = VK_ESC;
            } else if (ch == ' ') {
                type = VK_SPACE;
            }

            // Make action keys wider to fit text while keeping the last column aligned
            uint8_t width = (type == VK_BACKSPACE || type == VK_ENTER || type == VK_SPACE) ? (KEY_WIDTH * 3) : KEY_WIDTH;
            keyboard[row][col] = {ch, type, (uint8_t)(col * KEY_WIDTH), (uint8_t)(row * KEY_HEIGHT), width, KEY_HEIGHT};
        }
    }
}

void VirtualKeyboard::draw(OLEDDisplay *display, int16_t offsetX, int16_t offsetY)
{
    // Repeat ticking is driven by NotificationRenderer once per frame
    // Base styles
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    // Screen geometry
    const int screenW = display->getWidth();
    const int screenH = display->getHeight();

    // Decide wide-screen mode: if there is comfortable width, allow taller keys and reserve fixed width for last column labels
    // Heuristic: if screen width >= 200px (e.g., 240x135), treat as wide
    const bool isWide = screenW >= 200;

    // Determine last-column label max width
    display->setFont(FONT_SMALL);
    const int wENTER = display->getStringWidth("ENTER");
    int lastColLabelW = wENTER; // ENTER is usually the widest
    // Smaller padding on very small screens to avoid excessive whitespace
    const int lastColPad = (screenW <= 128 ? 2 : 6);
    const int reservedLastColW = lastColLabelW + lastColPad; // reserved width for last column keys

    // Always reserve width for the rightmost text column to avoid overlap on small screens
    int cellW = 0;
    int leftoverW = 0;
    {
        const int leftCols = KEYBOARD_COLS - 1; // 10 input characters
        int usableW = screenW - reservedLastColW;
        if (usableW < leftCols) {
            // Guard: ensure at least 1px per left cell if labels are extremely wide (unlikely)
            usableW = leftCols;
        }
        cellW = usableW / leftCols;
        leftoverW = usableW - cellW * leftCols; // distribute extra pixels over left columns (left to right)
    }

    // Dynamic key geometry
    int cellH = KEY_HEIGHT;
    int keyboardStartY = 0;
    if (screenH <= 64) {
        const int headerHeight = headerText.empty() ? 0 : (FONT_HEIGHT_SMALL - 2);
        const int gapBelowHeader = 0;
        const int singleLineBoxHeight = FONT_HEIGHT_SMALL;
        const int gapAboveKeyboard = 0;
        keyboardStartY = offsetY + headerHeight + gapBelowHeader + singleLineBoxHeight + gapAboveKeyboard;
        if (keyboardStartY < 0)
            keyboardStartY = 0;
        if (keyboardStartY > screenH)
            keyboardStartY = screenH;
        int keyboardHeight = screenH - keyboardStartY;
        cellH = std::max(1, keyboardHeight / KEYBOARD_ROWS);
    } else if (isWide) {
        // For wide screens (e.g., T114 240x135), prefer square keys: height equals left-column key width.
        cellH = std::max((int)KEY_HEIGHT, cellW);

        // Guarantee at least 2 lines of input are visible by reducing cell height minimally if needed.
        // Replicate the spacing used in drawInputArea(): headerGap=1, box-to-header gap=1, gap above keyboard=1
        display->setFont(FONT_SMALL);
        const int headerHeight = headerText.empty() ? 0 : (FONT_HEIGHT_SMALL + 1);
        const int headerToBoxGap = 1;
        const int gapAboveKb = 1;
        const int minBoxHeightForTwoLines = 2 * FONT_HEIGHT_SMALL + 2; // inner 1px top/bottom
        int maxKeyboardHeight = screenH - (offsetY + headerHeight + headerToBoxGap + minBoxHeightForTwoLines + gapAboveKb);
        int maxCellHAllowed = maxKeyboardHeight / KEYBOARD_ROWS;
        if (maxCellHAllowed < (int)KEY_HEIGHT)
            maxCellHAllowed = KEY_HEIGHT;
        if (maxCellHAllowed > 0 && cellH > maxCellHAllowed) {
            cellH = maxCellHAllowed;
        }
        // Keyboard placement from bottom for wide screens
        int keyboardHeight = KEYBOARD_ROWS * cellH;
        keyboardStartY = screenH - keyboardHeight;
        if (keyboardStartY < 0)
            keyboardStartY = 0;
    } else {
        // Default (non-wide, non-64px) e.g. SH1107 128x128:
        // cellH = FONT_HEIGHT_SMALL - 2 so rows are tighter while still hosting the font
        cellH = std::max((int)KEY_HEIGHT, FONT_HEIGHT_SMALL - 2);
        int keyboardHeight = KEYBOARD_ROWS * cellH;
        keyboardStartY = screenH - keyboardHeight;
        if (keyboardStartY < 0)
            keyboardStartY = 0;
    }

    // Draw input area above keyboard
    drawInputArea(display, offsetX, offsetY, keyboardStartY);

    // Precompute per-column x and width with leftover distributed over left columns for even spacing
    int colX[KEYBOARD_COLS];
    int colW[KEYBOARD_COLS];
    int runningX = offsetX;
    for (int col = 0; col < KEYBOARD_COLS - 1; ++col) {
        int wcol = cellW + (col < leftoverW ? 1 : 0);
        colX[col] = runningX;
        colW[col] = wcol;
        runningX += wcol;
    }
    // Last column
    colX[KEYBOARD_COLS - 1] = runningX;
    colW[KEYBOARD_COLS - 1] = reservedLastColW;

    // Draw keyboard grid
    for (int row = 0; row < KEYBOARD_ROWS; row++) {
        for (int col = 0; col < KEYBOARD_COLS; col++) {
            const VirtualKey &k = keyboard[row][col];
            const bool isLastCol = (col == KEYBOARD_COLS - 1);
            int x = colX[col];
            int w = colW[col];
            int y = offsetY + keyboardStartY + row * cellH;
            int h = cellH;
            bool selected = (row == cursorRow && col == cursorCol);
            if (strokeMode && !isLastCol) {
                std::string label = keyLabel(row, col);
                if (!label.empty())
                    drawLabeledKey(display, label, false, selected, x, y, (uint8_t)w, (uint8_t)h, false);
            } else if (k.character != 0 || k.type != VK_CHAR) {
                drawKey(display, k, selected, x, y, (uint8_t)w, (uint8_t)h, isLastCol);
            }
        }
    }
}

void VirtualKeyboard::drawInputArea(OLEDDisplay *display, int16_t offsetX, int16_t offsetY, int16_t keyboardStartY)
{
    display->setColor(WHITE);

    const int screenWidth = display->getWidth();
    const int screenHeight = display->getHeight();
    // Use the standard small font metrics for input box sizing (restore original size)
    const int inputLineH = FONT_HEIGHT_SMALL;

    // Header uses the standard small (which may be larger on big screens)
    display->setFont(FONT_SMALL);
    int headerHeight = 0;
    std::string header = headerText;
#if HAS_FONTPACKS
    // Stroke mode: the strokes typed so far after the header
    if (strokeMode) {
        header += header.empty() ? "筆 " : "  筆 ";
        for (int i = 0; i < StrokeInput::strokeCount(); i++) {
            uint8_t st = StrokeInput::strokeAt(i);
            header += (st >= 1 && st <= 6) ? kStrokeRow[st - 1] : "";
        }
    }
#endif
    if (!header.empty()) {
        // Draw header and reserve exact font height (plus a tighter gap) to maximize input area
        drawText(display, offsetX + 2, offsetY, header);
        if (screenHeight <= 64) {
            headerHeight = FONT_HEIGHT_SMALL - 2; // 11px
        } else {
            headerHeight = FONT_HEIGHT_SMALL; // no extra padding baked in
        }
    }

    const int boxX = offsetX;
    const int boxWidth = screenWidth;
    int boxY;
    int boxHeight;
    if (screenHeight <= 64) {
        const int gapBelowHeader = 0;
        const int fixedBoxHeight = inputLineH;
        const int gapAboveKeyboard = 0;
        boxY = offsetY + headerHeight + gapBelowHeader;
        boxHeight = fixedBoxHeight;
        if (boxY + boxHeight + gapAboveKeyboard > keyboardStartY) {
            int over = boxY + boxHeight + gapAboveKeyboard - keyboardStartY;
            boxHeight = std::max(1, fixedBoxHeight - over);
        }
    } else {
        const int gapBelowHeader = 1;
        int gapAboveKeyboard = 1;
        int tmpBoxY = offsetY + headerHeight + gapBelowHeader;
        const int minBoxHeight = inputLineH + 2;
        int availableH = keyboardStartY - tmpBoxY - gapAboveKeyboard;
        if (availableH < minBoxHeight)
            availableH = minBoxHeight;
        boxY = tmpBoxY;
        boxHeight = availableH;
    }

    // Draw box border
    display->drawRect(boxX, boxY, boxWidth, boxHeight);

    display->setFont(FONT_SMALL);

    // Text rendering: multi-line if space allows (>= 2 lines), else single-line with leading ellipsis
    const int textX = boxX + 2;
    const int maxTextWidth = boxWidth - 4;
    const int maxLines = (boxHeight - 2) / inputLineH;
    if (maxLines >= 2) {
        // Inner bounds for caret clamping
        const int innerLeft = boxX + 1;
        const int innerRight = boxX + boxWidth - 2;
        const int innerTop = boxY + 1;
        const int innerBottom = boxY + boxHeight - 2;

        // Wrap text greedily into lines that fit maxTextWidth
        std::vector<std::string> lines;
        {
            std::string remaining = inputText;
            while (!remaining.empty()) {
                // Whole UTF-8 characters: a CJK character is 3 bytes
                size_t bestLen = 0;
                size_t len = 0;
                while (len < remaining.size()) {
                    size_t next = utf8NextBoundary(remaining.c_str(), remaining.size(), len);
                    if (textWidth(display, remaining.substr(0, next)) > maxTextWidth)
                        break;
                    bestLen = next;
                    len = next;
                }
                if (bestLen == 0) {
                    // At least show one character to make progress
                    bestLen = utf8NextBoundary(remaining.c_str(), remaining.size(), 0);
                }
                lines.emplace_back(remaining.substr(0, bestLen));
                remaining.erase(0, bestLen);
            }
        }

        const bool scrolledUp = ((int)lines.size() > maxLines);
        int caretX = textX;
        int caretY = innerTop;

        // Leave a small top gap to render '...' without replacing the first line
        const int topInset = 2;
        const int lineStep = std::max(1, inputLineH - 1); // slightly tighter than font height
        int lineY = innerTop + topInset;

        if (scrolledUp) {
            // Draw three small dots centered horizontally, vertically at the midpoint of the gap
            // between the inner top and the first line's top baseline. This avoids using a tall glyph.
            const int firstLineTop = lineY;                                   // baseline top for the first visible line
            const int gapMidY = innerTop + (firstLineTop - innerTop) / 2 + 1; // shift down 1px as requested
            const int centerX = boxX + boxWidth / 2;
            const int dotSpacing = 3; // px between dots
            const int dotSize = 1;    // small square dot
            display->fillRect(centerX - dotSpacing, gapMidY, dotSize, dotSize);
            display->fillRect(centerX, gapMidY, dotSize, dotSize);
            display->fillRect(centerX + dotSpacing, gapMidY, dotSize, dotSize);
        }

        // How many lines fit with our top inset and tighter step
        const int linesCapacity = std::max(1, (innerBottom - lineY + 1) / lineStep);
        const int linesToShow = std::min((int)lines.size(), linesCapacity);
        const int startIndex = scrolledUp ? ((int)lines.size() - linesToShow) : 0;

        for (int i = 0; i < linesToShow; ++i) {
            const std::string &chunk = lines[startIndex + i];
            drawText(display, textX, lineY, chunk);
            caretX = textX + textWidth(display, chunk);
            caretY = lineY;
            lineY += lineStep;
        }

        // Draw caret at end of the last visible line
        int caretPadY = 2;
        if (boxHeight >= inputLineH + 4)
            caretPadY = 3;
        int cursorTop = caretY + caretPadY;
        // Use lineStep so caret height matches the row spacing
        int cursorH = lineStep - caretPadY * 2;
        if (cursorH < 1)
            cursorH = 1;
        // Clamp vertical bounds to stay inside the inner rect
        if (cursorTop < innerTop)
            cursorTop = innerTop;
        if (cursorTop + cursorH - 1 > innerBottom)
            cursorH = innerBottom - cursorTop + 1;
        if (cursorH < 1)
            cursorH = 1;
        // Only draw if cursor is inside inner bounds
        if (caretX >= innerLeft && caretX <= innerRight) {
            display->drawVerticalLine(caretX, cursorTop, cursorH);
        }
    } else {
        std::string displayText = inputText;
        int textW = textWidth(display, displayText);
        std::string scrolled = displayText;
        if (textW > maxTextWidth) {
            // Trim whole characters from the left until it fits
            while (textW > maxTextWidth && !scrolled.empty()) {
                scrolled.erase(0, utf8NextBoundary(scrolled.c_str(), scrolled.size(), 0));
                textW = textWidth(display, scrolled);
            }
            // Add leading ellipsis and ensure it still fits
            if (scrolled != displayText) {
                scrolled = "..." + scrolled;
                textW = textWidth(display, scrolled);
                // If adding ellipsis causes overflow, trim more after the ellipsis
                while (textW > maxTextWidth && scrolled.size() > 3) {
                    scrolled.erase(3, utf8NextBoundary(scrolled.c_str() + 3, scrolled.size() - 3, 0));
                    textW = textWidth(display, scrolled);
                }
            }
        } else {
            // Keep textW in sync with what we draw
            textW = textWidth(display, scrolled);
        }

        int textY;
        if (screenHeight <= 64) {
            textY = boxY + (boxHeight - inputLineH) / 2;
        } else {
            const int innerTop = boxY + 1;
            const int innerBottom = boxY + boxHeight - 2;

            // Center text vertically within inner box for single-line, then clamp so it never overlaps borders
            int innerH = innerBottom - innerTop + 1;
            textY = innerTop + std::max(0, (innerH - inputLineH) / 2);
            // Clamp fully inside the inner rect
            if (textY < innerTop)
                textY = innerTop;
            int maxTop = innerBottom - inputLineH + 1;
            if (textY > maxTop)
                textY = maxTop;
        }

        if (!scrolled.empty()) {
            drawText(display, textX, textY, scrolled);
        }

        int cursorX = textX + textW;
        if (screenHeight > 64) {
            const int innerRight = boxX + boxWidth - 2;
            if (cursorX > innerRight)
                cursorX = innerRight;
        }

        int cursorTop, cursorH;
        if (screenHeight <= 64) {
            cursorH = 10;
            cursorTop = boxY + (boxHeight - cursorH) / 2;
        } else {
            const int innerLeft = boxX + 1;
            const int innerRight = boxX + boxWidth - 2;
            const int innerTop = boxY + 1;
            const int innerBottom = boxY + boxHeight - 2;

            cursorTop = boxY + 2;
            cursorH = boxHeight - 4;
            if (cursorH < 1)
                cursorH = 1;
            if (cursorTop < innerTop)
                cursorTop = innerTop;
            if (cursorTop + cursorH - 1 > innerBottom)
                cursorH = innerBottom - cursorTop + 1;
            if (cursorH < 1)
                cursorH = 1;

            if (cursorX < innerLeft || cursorX > innerRight)
                return;
        }

        display->drawVerticalLine(cursorX, cursorTop, cursorH);
    }
}

void VirtualKeyboard::drawKey(OLEDDisplay *display, const VirtualKey &key, bool selected, int16_t x, int16_t y, uint8_t width,
                              uint8_t height, bool isLastCol)
{
    // The key's label, drawn by drawLabeledKey
    display->setFont(FONT_SMALL);
    std::string keyText;
    if (key.type == VK_BACKSPACE || key.type == VK_ENTER || key.type == VK_SPACE || key.type == VK_ESC) {
        // Keep literal text labels for the action keys on the rightmost column
        keyText = (key.type == VK_BACKSPACE) ? "BACK"
                  : (key.type == VK_ENTER)   ? "ENTER"
                  : (key.type == VK_SPACE)   ? "SPACE"
                  : (key.type == VK_ESC)     ? "ESC"
                                             : "";
    } else {
        char c = getCharForKey(key, false);
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
        keyText = (key.character == ' ' || key.character == '_') ? "_" : std::string(1, c);
        // Show the common "/" pairing next to "?" like on a real keyboard
        if (key.type == VK_CHAR && key.character == '?') {
            keyText = "?/";
        }
        // With a stroke dictionary, ';' switches to the strokes (a long press still types ';')
        if (strokesAvailable && key.type == VK_CHAR && key.character == ';') {
            keyText = "筆";
        }
    }
    const bool isAction = (key.type == VK_BACKSPACE || key.type == VK_ENTER || key.type == VK_SPACE || key.type == VK_ESC);
    drawLabeledKey(display, keyText, isAction, selected, x, y, width, height, isLastCol);
}

void VirtualKeyboard::drawLabeledKey(OLEDDisplay *display, const std::string &keyText, bool isAction, bool selected, int16_t x,
                                     int16_t y, uint8_t width, uint8_t height, bool isLastCol)
{
    display->setFont(FONT_SMALL);
    const int fontH = FONT_HEIGHT_SMALL;
    int textWidth = this->textWidth(display, keyText);
    // Label alignment
    // - Rightmost action column: right-align text with a small right padding (~2px) so it hugs screen edge neatly.
    // - Other keys: center horizontally; use ceil-style rounding to avoid appearing left-biased on odd widths.
    int textX;
    if (isLastCol) {
        const int rightPad = 1;
        textX = x + width - textWidth - rightPad;
        if (textX < x)
            textX = x; // guard
    } else {
        // Use ceil rounding for all screens (consistent with 128x64 behavior for numbers)
        textX = x + (width - textWidth + 1) / 2;
    }
    int contentTop = y;
    int contentH = height;
    if (selected) {
        display->setColor(WHITE);

        if (display->getHeight() <= 64 && !isAction) {
            display->fillRect(x, y, width, height);
        } else if (isAction) {
            const int padX = 1;
            const int padY = 2;
            int hlW = textWidth + padX * 2;
            int hlX = textX - padX;

            if (hlX < x) {
                hlW -= (x - hlX);
                hlX = x;
            }
            int maxW = (x + width) - hlX;
            if (hlW > maxW)
                hlW = maxW;
            if (hlW < 1)
                hlW = 1;

            int hlH = std::min(fontH + padY * 2, (int)height);
            int hlY = y + (height - hlH) / 2;
            display->fillRect(hlX, hlY, hlW, hlH);
            contentTop = hlY;
            contentH = hlH;
        } else {
            display->fillRect(x, y, width, height);
        }
        display->setColor(BLACK);
    } else {
        display->setColor(WHITE);
    }

    int centeredTextY;
    if (display->getHeight() <= 64) {
        centeredTextY = y + (height - fontH) / 2;
    } else {
        centeredTextY = contentTop + (contentH - fontH) / 2;
    }
    if (display->getHeight() > 64) {
        if (centeredTextY < contentTop)
            centeredTextY = contentTop;
        if (centeredTextY + fontH > contentTop + contentH)
            centeredTextY = std::max(contentTop, contentTop + contentH - fontH);
    }

    if (display->getHeight() <= 64 && keyText.size() == 1) {
        char ch = keyText[0];
        if (ch == '.' || ch == ',' || ch == ';') {
            centeredTextY -= 1;
        }
    }
#ifdef MUZI_BASE // Correct issue with character vertical position on MUZI_BASE
    centeredTextY -= 2;
#endif
    drawText(display, textX, centeredTextY, keyText);
}

char VirtualKeyboard::getCharForKey(const VirtualKey &key, bool isLongPress)
{
    if (key.type != VK_CHAR) {
        return key.character;
    }

    char c = key.character;

    // Long-press: letters become uppercase; for "?" provide "/" like a typical keyboard
    if (isLongPress) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        } else if (c == '?') {
            c = '/';
        }
    }

    return c;
}

void VirtualKeyboard::moveCursorDelta(int dRow, int dCol)
{
    resetTimeout();
    // wrap around rows and cols in the 4x11 grid
    int r = (int)cursorRow + dRow;
    int c = (int)cursorCol + dCol;
    if (r < 0)
        r = KEYBOARD_ROWS - 1;
    else if (r >= KEYBOARD_ROWS)
        r = 0;
    if (c < 0)
        c = KEYBOARD_COLS - 1;
    else if (c >= KEYBOARD_COLS)
        c = 0;
    cursorRow = (uint8_t)r;
    cursorCol = (uint8_t)c;
}

void VirtualKeyboard::moveCursorUp()
{
    moveCursorDelta(-1, 0);
}
void VirtualKeyboard::moveCursorDown()
{
    moveCursorDelta(1, 0);
}
void VirtualKeyboard::moveCursorLeft()
{
    resetTimeout();

    if (cursorCol > 0) {
        cursorCol--;
    } else {
        if (cursorRow > 0) {
            cursorRow--;
            cursorCol = KEYBOARD_COLS - 1;
        } else {
            cursorRow = KEYBOARD_ROWS - 1;
            cursorCol = KEYBOARD_COLS - 1;
        }
    }
}
void VirtualKeyboard::moveCursorRight()
{
    resetTimeout();

    if (cursorCol < KEYBOARD_COLS - 1) {
        cursorCol++;
    } else {
        if (cursorRow < KEYBOARD_ROWS - 1) {
            cursorRow++;
            cursorCol = 0;
        } else {
            cursorRow = 0;
            cursorCol = 0;
        }
    }
}

void VirtualKeyboard::handlePress()
{
    resetTimeout(); // Reset timeout on any input activity
    if (handleStrokePress(false))
        return;

    const VirtualKey &key = keyboard[cursorRow][cursorCol];

    // Don't handle press if the key is empty (but allow special keys)
    if (key.character == 0 && key.type == VK_CHAR) {
        return;
    }

    // For character keys, insert lowercase character
    if (key.type == VK_CHAR) {
        insertCharacter(getCharForKey(key, false)); // false = lowercase/normal char
        return;
    }

    // Handle non-character keys immediately
    switch (key.type) {
    case VK_BACKSPACE:
        deleteCharacter();
        break;
    case VK_ENTER:
        submitText();
        break;
    case VK_SPACE:
        insertCharacter(' ');
        break;
    case VK_ESC:
        if (onTextEntered) {
            std::function<void(const std::string &)> callback = onTextEntered;
            onTextEntered = nullptr;
            inputText = "";
            callback("");
        }
        return;
    default:
        break;
    }
}

void VirtualKeyboard::handleLongPress()
{
    resetTimeout(); // Reset timeout on any input activity
    if (strokeMode && handleStrokePress(true))
        return;

    const VirtualKey &key = keyboard[cursorRow][cursorCol];

    // Don't handle press if the key is empty (but allow special keys)
    if (key.character == 0 && key.type == VK_CHAR) {
        return;
    }

    // For character keys, insert uppercase/alternate character
    if (key.type == VK_CHAR) {
        insertCharacter(getCharForKey(key, true)); // true = uppercase/alternate char
        return;
    }

    switch (key.type) {
    case VK_BACKSPACE:
        // One-shot: delete up to 5 characters on long press
        for (int i = 0; i < 5; ++i) {
            if (inputText.empty())
                break;
            deleteCharacter();
        }
        break;
    case VK_ENTER:
        submitText();
        break;
    case VK_SPACE:
        insertCharacter(' ');
        break;
    case VK_ESC:
        if (onTextEntered) {
            // Copy-and-clear before invoking, like handlePress/submitText: the callback can
            // destroy this keyboard (OnScreenKeyboardModule::stop), so the member must not be
            // the std::function still executing on the stack.
            std::function<void(const std::string &)> callback = onTextEntered;
            onTextEntered = nullptr;
            inputText = "";
            callback("");
        }
        break;
    default:
        break;
    }
}

void VirtualKeyboard::insertCharacter(char c)
{
    insertText(std::string(1, c));
}

bool VirtualKeyboard::insertText(const std::string &text)
{
    if (inputText.length() + text.length() > 160) // Reasonable text length limit
        return false;
    inputText += text;
    return true;
}

void VirtualKeyboard::deleteCharacter()
{
    // The whole last character (a CJK character is 3 bytes)
    if (!inputText.empty()) {
        inputText.erase(utf8PrevBoundary(inputText.c_str(), inputText.size(), inputText.size()));
    }
}

int VirtualKeyboard::textWidth(OLEDDisplay *display, const std::string &s)
{
#if HAS_FONTPACKS
    return EmoteRenderer::measureStringWithEmotes(display, s);
#else
    return display->getStringWidth(s.c_str());
#endif
}

void VirtualKeyboard::drawText(OLEDDisplay *display, int x, int y, const std::string &s)
{
#if HAS_FONTPACKS
    EmoteRenderer::drawStringWithEmotes(display, x, y, s, FONT_HEIGHT_SMALL, emotes, numEmotes, 1, false);
#else
    display->drawString(x, y, s.c_str());
#endif
}

std::string VirtualKeyboard::keyLabel(int row, int col)
{
#if HAS_FONTPACKS
    if (!strokeMode || col >= KEYBOARD_COLS - 1)
        return "";
    if (row == 0)
        return col < (int)(sizeof(kStrokeRow) / sizeof(kStrokeRow[0])) ? kStrokeRow[col] : "";
    if (row == 3)
        return kPunctRow[col];
    uint32_t cp = StrokeInput::candidateAt(candidatePage * kCandidatesPerPage + (row - 1) * 10 + col);
    return cp ? WaypointUtils::utf8FromCodepoint(cp) : "";
#else
    (void)row;
    (void)col;
    return "";
#endif
}

void VirtualKeyboard::pickCandidate(int index)
{
#if HAS_FONTPACKS
    // Only when it fits: otherwise the strokes stay, and the user can delete text to make room
    uint32_t cp = StrokeInput::candidateAt(index);
    if (cp && insertText(WaypointUtils::utf8FromCodepoint(cp))) {
        StrokeInput::pickAt(index);
        candidatePage = 0;
        // Back to the first stroke for the next character
        cursorRow = 0;
        cursorCol = 0;
    }
#else
    (void)index;
#endif
}

bool VirtualKeyboard::handleStrokePress(bool isLongPress)
{
#if HAS_FONTPACKS
    const VirtualKey &key = keyboard[cursorRow][cursorCol];
    if (!strokeMode) {
        // Letters: the ';' key goes to the strokes; a long press still types ';'
        if (strokesAvailable && !isLongPress && key.type == VK_CHAR && key.character == ';') {
            strokeMode = true;
            candidatePage = 0;
            StrokeInput::reset();
            cursorRow = 0;
            cursorCol = 0;
            return true;
        }
        return false;
    }

    const bool composing = StrokeInput::strokeCount() > 0;
    if (cursorCol == KEYBOARD_COLS - 1) {
        // The action keys: while strokes are typed, BACK removes one, ENTER and SPACE take the first candidate shown,
        // ESC clears them; otherwise they act as in letter mode
        if (!composing)
            return false;
        switch (key.type) {
        case VK_BACKSPACE:
            StrokeInput::removeStroke();
            candidatePage = 0;
            return true;
        case VK_ENTER:
        case VK_SPACE:
            pickCandidate(candidatePage * kCandidatesPerPage);
            return true;
        case VK_ESC:
            StrokeInput::reset();
            candidatePage = 0;
            return true;
        default:
            return false;
        }
    }

    switch (cursorRow) {
    case 0:
        if (cursorCol <= 5) {
            static const uint8_t stroke[] = {1, 2, 3, 4, 5, StrokeInput::kAnyStroke}; // 横 竖 撇 点 折, any
            StrokeInput::addStroke(stroke[cursorCol]);
            candidatePage = 0;
        } else if (cursorCol == 6) {
            if (candidatePage > 0)
                candidatePage--;
        } else if (cursorCol == 7) {
            if ((candidatePage + 1) * kCandidatesPerPage < StrokeInput::candidateCount())
                candidatePage++;
        } else if (cursorCol == 8) {
            // EN: back to letters, on the 筆 key
            strokeMode = false;
            StrokeInput::reset();
            cursorRow = 2;
            cursorCol = 9;
        }
        return true;
    case 1:
    case 2:
        pickCandidate(candidatePage * kCandidatesPerPage + (cursorRow - 1) * 10 + cursorCol);
        return true;
    default:
        insertText(kPunctRow[cursorCol]);
        return true;
    }
#else
    (void)isLongPress;
    return false;
#endif
}

void VirtualKeyboard::submitText()
{
    LOG_INFO("Virtual keyboard: submitting text '%s'", inputText.c_str());

    // Only submit if text is not empty
    if (!inputText.empty() && onTextEntered) {
        // Store callback and text to submit before clearing callback
        std::function<void(const std::string &)> callback = onTextEntered;
        std::string textToSubmit = inputText;
        onTextEntered = nullptr;
        // Don't clear inputText here - let the calling module handle cleanup
        // inputText = "";  // Removed: keep text visible until module cleans up
        callback(textToSubmit);
    } else if (inputText.empty()) {
        // For empty text, just ignore the submission - don't clear callback
        // This keeps the virtual keyboard responsive for further input
        LOG_INFO("Virtual keyboard: empty text submitted, ignoring - keyboard remains active");
    } else {
        // No callback available
        if (screen) {
            screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
        }
    }
}

void VirtualKeyboard::setInputText(const std::string &text)
{
    inputText = text;
}

void VirtualKeyboard::setHeader(const std::string &header)
{
    headerText = header;
}

void VirtualKeyboard::setCallback(std::function<void(const std::string &)> callback)
{
    onTextEntered = callback;
}

void VirtualKeyboard::resetTimeout()
{
    lastActivityTime = millis();
}

bool VirtualKeyboard::isTimedOut() const
{
    return (millis() - lastActivityTime) > TIMEOUT_MS;
}

} // namespace graphics
#endif
