#include "configuration.h"
#if HAS_SCREEN

#include "graphics/EmoteRenderer.h"
#include "graphics/fontpack/FontPacks.h"
#include <algorithm>
#include <cstring>

namespace graphics
{
namespace EmoteRenderer
{

static inline int getStringWidth(OLEDDisplay *display, const char *text, size_t len)
{
#if defined(OLED_UA) || defined(OLED_RU)
    return display->getStringWidth(text, len, true);
#else
    // OLEDDisplay::getStringWidth (utf8=false) indexes the font jump table by (c - firstChar) with a
    // signed char and no bounds check, so any byte outside printable ASCII (high bytes, but also
    // control bytes like a stray 0x0A) reads outside the font array. Measure a sanitized copy in which
    // unrepresentable bytes count as a '?' placeholder; printable ASCII is passed through unchanged.
    char safe[64];
    if (len > sizeof(safe) - 1)
        len = sizeof(safe) - 1;
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        safe[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
    }
    safe[len] = '\0';
    return display->getStringWidth(safe, len, false);
#endif
}

size_t utf8CharLen(uint8_t c)
{
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

uint32_t utf8CodePoint(const char *s, size_t len)
{
    const uint8_t c = static_cast<uint8_t>(s[0]);
    const size_t n = utf8CharLen(c);
    if (n > len)
        return 0xFFFD;
    if (n == 1)
        return c;
    uint32_t cp = c & (0x7F >> n);
    for (size_t i = 1; i < n; i++)
        cp = (cp << 6) | (static_cast<uint8_t>(s[i]) & 0x3F);
    return cp;
}

bool canBreakBefore(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) || // CJK radicals, punctuation, kana, Bopomofo, ideographs
           (cp >= 0xA960 && cp <= 0xA97F) || // Hangul jamo extended-A
           (cp >= 0xAC00 && cp <= 0xD7FF) || // Hangul syllables, jamo extended-B
           (cp >= 0xF900 && cp <= 0xFAFF) || // CJK compatibility ideographs
           (cp >= 0xFE30 && cp <= 0xFE4F) || // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFFEF) || // halfwidth and fullwidth forms
           (cp >= 0x20000 && cp <= 0x3FFFF); // CJK ideographs, supplementary planes
}

// Bytes the UTF-8 char at s[pos] occupies, clamped to what actually remains. A truncated multi-byte
// lead (e.g. a lone 0xF0) near the end otherwise makes a walker read past the buffer - the payload of
// a TEXT message is opaque bytes, so such truncated sequences are attacker-reachable.
static inline size_t utf8CharLenClamped(const char *s, size_t pos, size_t len)
{
    return std::min(utf8CharLen(static_cast<uint8_t>(s[pos])), len - pos);
}

static inline bool isPossibleEmoteLead(uint8_t c)
{
    // All supported emoji labels in emotes.cpp are currently in these UTF-8 lead ranges.
    return c == 0xE2 || c == 0xF0;
}

// True when the character at s[pos] is drawn from the font packs (the built-in font doesn't have it)
static inline bool isPackCharAt(const char *s, size_t pos, size_t len, uint32_t &cp, size_t &charLen)
{
#if HAS_FONTPACKS
    if (static_cast<uint8_t>(s[pos]) < 0x80)
        return false;
    cp = FontPacks::decodeUtf8(s + pos, len - pos, charLen);
    return FontPacks::handles(cp);
#else
    (void)s, (void)pos, (void)len, (void)cp, (void)charLen;
    return false;
#endif
}

static inline int getUtf8ChunkWidth(OLEDDisplay *display, const char *text, size_t len)
{
#if HAS_FONTPACKS
    uint32_t cp;
    size_t charLen;
    if (isPackCharAt(text, 0, len, cp, charLen))
        return FontPacks::advance(cp);
#endif
    char chunk[5] = {0, 0, 0, 0, 0};
    if (len > 4)
        len = 4;
    memcpy(chunk, text, len);
    return getStringWidth(display, chunk, len);
}

static inline bool isFE0FAt(const char *s, size_t pos, size_t len)
{
    return pos + 2 < len && static_cast<uint8_t>(s[pos]) == 0xEF && static_cast<uint8_t>(s[pos + 1]) == 0xB8 &&
           static_cast<uint8_t>(s[pos + 2]) == 0x8F;
}

static inline bool isSkinToneAt(const char *s, size_t pos, size_t len)
{
    return pos + 3 < len && static_cast<uint8_t>(s[pos]) == 0xF0 && static_cast<uint8_t>(s[pos + 1]) == 0x9F &&
           static_cast<uint8_t>(s[pos + 2]) == 0x8F &&
           (static_cast<uint8_t>(s[pos + 3]) >= 0xBB && static_cast<uint8_t>(s[pos + 3]) <= 0xBF);
}

static inline size_t ignorableModifierLenAt(const char *s, size_t pos, size_t len)
{
    // Skip modifiers that do not change which bitmap we render.
    if (isFE0FAt(s, pos, len))
        return 3;
    if (isSkinToneAt(s, pos, len))
        return 4;
    return 0;
}

const Emote *findEmoteByLabel(const char *label, const Emote *emoteSet, int emoteCount)
{
    if (!label || !*label)
        return nullptr;

    for (int i = 0; i < emoteCount; ++i) {
        if (emoteSet[i].label && strcmp(label, emoteSet[i].label) == 0)
            return &emoteSet[i];
    }

    return nullptr;
}

static bool matchAtIgnoringModifiers(const char *text, size_t textLen, size_t pos, const char *label, size_t &textConsumed,
                                     size_t &matchScore)
{
    // Treat FE0F and skin-tone modifiers as optional while matching.
    textConsumed = 0;
    matchScore = 0;
    if (!label || !*label || pos >= textLen)
        return false;

    const size_t labelLen = strlen(label);
    size_t ti = pos;
    size_t li = 0;

    while (true) {
        while (ti < textLen) {
            const size_t skipLen = ignorableModifierLenAt(text, ti, textLen);
            if (!skipLen)
                break;
            ti += skipLen;
        }
        while (li < labelLen) {
            const size_t skipLen = ignorableModifierLenAt(label, li, labelLen);
            if (!skipLen)
                break;
            li += skipLen;
        }

        if (li >= labelLen) {
            while (ti < textLen) {
                const size_t skipLen = ignorableModifierLenAt(text, ti, textLen);
                if (!skipLen)
                    break;
                ti += skipLen;
            }
            textConsumed = ti - pos;
            return textConsumed > 0;
        }

        if (ti >= textLen)
            return false;

        const uint8_t tc = static_cast<uint8_t>(text[ti]);
        const uint8_t lc = static_cast<uint8_t>(label[li]);
        const size_t tlen = utf8CharLen(tc);
        const size_t llen = utf8CharLen(lc);

        if (tlen != llen || ti + tlen > textLen || li + llen > labelLen)
            return false;
        if (memcmp(text + ti, label + li, tlen) != 0)
            return false;

        ti += tlen;
        li += llen;
        matchScore += llen;
    }
}

const Emote *findEmoteAt(const char *text, size_t textLen, size_t pos, size_t &matchLen, const Emote *emoteSet, int emoteCount)
{
    // Prefer the longest matching label at this byte offset.
    const Emote *matched = nullptr;
    matchLen = 0;
    size_t bestScore = 0;
    if (!text || pos >= textLen)
        return nullptr;

    if (!isPossibleEmoteLead(static_cast<uint8_t>(text[pos])))
        return nullptr;

    for (int i = 0; i < emoteCount; ++i) {
        const char *label = emoteSet[i].label;
        if (!label || !*label)
            continue;
        if (static_cast<uint8_t>(label[0]) != static_cast<uint8_t>(text[pos]))
            continue;

        const size_t labelLen = strlen(label);
        if (labelLen == 0)
            continue;

        size_t candidateLen = 0;
        size_t candidateScore = 0;
        if (pos + labelLen <= textLen && memcmp(text + pos, label, labelLen) == 0) {
            candidateLen = labelLen;
            candidateScore = labelLen;
        } else if (matchAtIgnoringModifiers(text, textLen, pos, label, candidateLen, candidateScore)) {
            // Matched with FE0F/skin tone modifiers treated as optional.
        } else {
            continue;
        }

        if (candidateScore > bestScore) {
            matched = &emoteSet[i];
            matchLen = candidateLen;
            bestScore = candidateScore;
        }
    }

    return matched;
}

static LineMetrics analyzeLineInternal(OLEDDisplay *display, const char *line, size_t lineLen, int fallbackHeight,
                                       const Emote *emoteSet, int emoteCount, int emoteSpacing)
{
    // Scan once to collect width and tallest emote for this line.
    LineMetrics metrics{0, fallbackHeight, false, false};
    if (!line)
        return metrics;

    for (size_t i = 0; i < lineLen;) {
        size_t matchLen = 0;
        const Emote *matched = findEmoteAt(line, lineLen, i, matchLen, emoteSet, emoteCount);
        if (matched) {
            metrics.hasEmote = true;
            metrics.tallestHeight = std::max(metrics.tallestHeight, matched->height);
            if (display)
                metrics.width += matched->width + emoteSpacing;
            i += matchLen;
            continue;
        }

        const size_t skipLen = ignorableModifierLenAt(line, i, lineLen);
        if (skipLen) {
            i += skipLen;
            continue;
        }

        const size_t charLen = utf8CharLenClamped(line, i, lineLen);
        uint32_t cp;
        size_t packLen;
        if (isPackCharAt(line, i, lineLen, cp, packLen))
            metrics.hasPackGlyph = true;
        if (display)
            metrics.width += getUtf8ChunkWidth(display, line + i, charLen);
        i += charLen;
    }

    return metrics;
}

LineMetrics analyzeLine(OLEDDisplay *display, const char *line, int fallbackHeight, const Emote *emoteSet, int emoteCount,
                        int emoteSpacing)
{
    return analyzeLineInternal(display, line, line ? strlen(line) : 0, fallbackHeight, emoteSet, emoteCount, emoteSpacing);
}

int maxEmoteHeight(const Emote *emoteSet, int emoteCount)
{
    int tallest = 0;
    for (int i = 0; i < emoteCount; ++i) {
        if (emoteSet[i].label && *emoteSet[i].label)
            tallest = std::max(tallest, emoteSet[i].height);
    }
    return tallest;
}

int measureStringWithEmotes(OLEDDisplay *display, const char *line, const Emote *emoteSet, int emoteCount, int emoteSpacing)
{
    if (!display)
        return 0;

    if (!line || !*line)
        return 0;

    return analyzeLine(display, line, 0, emoteSet, emoteCount, emoteSpacing).width;
}

static int appendTextSpanAndMeasure(OLEDDisplay *display, int cursorX, int fontY, int fontHeight, const char *text, size_t len,
                                    bool draw, bool fauxBold)
{
    // Draw plain-text runs in chunks so UTF-8 stays intact.
    if (!text || len == 0)
        return cursorX;

    char chunk[33];
    size_t pos = 0;
    while (pos < len) {
#if HAS_FONTPACKS
        // A character the built-in font doesn't have comes from the font packs
        uint32_t cp;
        size_t packLen;
        if (isPackCharAt(text, pos, len, cp, packLen)) {
            if (draw) {
                if (fauxBold)
                    FontPacks::draw(display, cursorX + 1, fontY, fontHeight, cp);
                cursorX += FontPacks::draw(display, cursorX, fontY, fontHeight, cp);
            } else {
                cursorX += FontPacks::advance(cp);
            }
            pos += packLen;
            continue;
        }
#else
        (void)fontHeight;
#endif

        size_t chunkLen = 0;
        while (pos + chunkLen < len) {
#if HAS_FONTPACKS
            // End the chunk before a character from the packs
            if (chunkLen > 0 && isPackCharAt(text, pos + chunkLen, len, cp, packLen))
                break;
#endif
            const size_t charLen = utf8CharLenClamped(text, pos + chunkLen, len);
            if (chunkLen + charLen >= sizeof(chunk))
                break;
            chunkLen += charLen;
        }

        if (chunkLen == 0) {
            chunkLen = std::min(len - pos, sizeof(chunk) - 1);
        }

        memcpy(chunk, text + pos, chunkLen);
        chunk[chunkLen] = '\0';
        if (draw) {
            if (fauxBold)
                display->drawString(cursorX + 1, fontY, chunk);
            display->drawString(cursorX, fontY, chunk);
        }
        cursorX += getStringWidth(display, chunk, chunkLen);
        pos += chunkLen;
    }

    return cursorX;
}

size_t truncateToWidth(OLEDDisplay *display, const char *line, char *out, size_t outSize, int maxWidth, const char *ellipsis,
                       const Emote *emoteSet, int emoteCount, int emoteSpacing)
{
    if (!out || outSize == 0)
        return 0;

    out[0] = '\0';
    if (!display || !line || maxWidth <= 0)
        return 0;

    const size_t lineLen = strlen(line);
    const int suffixWidth =
        (ellipsis && *ellipsis) ? measureStringWithEmotes(display, ellipsis, emoteSet, emoteCount, emoteSpacing) : 0;
    const char *suffix = (ellipsis && suffixWidth <= maxWidth) ? ellipsis : "";
    const size_t suffixLen = strlen(suffix);
    const int availableWidth = maxWidth - (*suffix ? suffixWidth : 0);

    if (measureStringWithEmotes(display, line, emoteSet, emoteCount, emoteSpacing) <= maxWidth) {
        strncpy(out, line, outSize - 1);
        out[outSize - 1] = '\0';
        return strlen(out);
    }

    int used = 0;
    size_t cut = 0;
    for (size_t i = 0; i < lineLen;) {
        // Keep whole emotes together when deciding where to cut.
        int tokenWidth = 0;
        size_t advance = 0;

        if (isPossibleEmoteLead(static_cast<uint8_t>(line[i]))) {
            size_t matchLen = 0;
            const Emote *matched = findEmoteAt(line, lineLen, i, matchLen, emoteSet, emoteCount);
            if (matched) {
                tokenWidth = matched->width + emoteSpacing;
                advance = matchLen;
            }
        }

        if (advance == 0) {
            const size_t skipLen = ignorableModifierLenAt(line, i, lineLen);
            if (skipLen) {
                i += skipLen;
                cut = i;
                continue;
            }

            const size_t charLen = utf8CharLenClamped(line, i, lineLen);
            tokenWidth = getUtf8ChunkWidth(display, line + i, charLen);
            advance = charLen;
        }

        if (used + tokenWidth > availableWidth)
            break;

        used += tokenWidth;
        i += advance;
        cut = i;
    }

    if (cut == 0) {
        strncpy(out, suffix, outSize - 1);
        out[outSize - 1] = '\0';
        return strlen(out);
    }

    size_t copyLen = cut;
    if (copyLen > outSize - 1)
        copyLen = outSize - 1;
    if (suffixLen > 0 && copyLen + suffixLen > outSize - 1) {
        copyLen = (outSize - 1 > suffixLen) ? (outSize - 1 - suffixLen) : 0;
    }

    memcpy(out, line, copyLen);
    size_t totalLen = copyLen;
    if (suffixLen > 0 && totalLen < outSize - 1) {
        memcpy(out + totalLen, suffix, std::min(suffixLen, outSize - 1 - totalLen));
        totalLen += std::min(suffixLen, outSize - 1 - totalLen);
    }
    out[totalLen] = '\0';
    return totalLen;
}

void drawStringWithEmotes(OLEDDisplay *display, int x, int y, const char *line, int fontHeight, const Emote *emoteSet,
                          int emoteCount, int emoteSpacing, bool fauxBold)
{
    if (!line)
        return;

    const size_t lineLen = strlen(line);
    // Center text vertically when any emote is taller than the font.
    const int maxIconHeight =
        analyzeLineInternal(nullptr, line, lineLen, fontHeight, emoteSet, emoteCount, emoteSpacing).tallestHeight;
    const int lineHeight = std::max(fontHeight, maxIconHeight);
    const int fontY = y + (lineHeight - fontHeight) / 2;

    int cursorX = x;
    bool inBold = false;

    for (size_t i = 0; i < lineLen;) {
        // Toggle faux bold.
        if (fauxBold && i + 1 < lineLen && line[i] == '*' && line[i + 1] == '*') {
            inBold = !inBold;
            i += 2;
            continue;
        }

        const size_t skipLen = ignorableModifierLenAt(line, i, lineLen);
        if (skipLen) {
            i += skipLen;
            continue;
        }

        size_t matchLen = 0;
        const Emote *matched = findEmoteAt(line, lineLen, i, matchLen, emoteSet, emoteCount);
        if (matched) {
            const int iconY = y + (lineHeight - matched->height) / 2;
            display->drawXbm(cursorX, iconY, matched->width, matched->height, matched->bitmap);
            cursorX += matched->width + emoteSpacing;
            i += matchLen;
            continue;
        }

        size_t next = i;
        while (next < lineLen) {
            // Stop the text run before the next emote or bold marker.
            if (fauxBold && next + 1 < lineLen && line[next] == '*' && line[next + 1] == '*')
                break;

            if (ignorableModifierLenAt(line, next, lineLen))
                break;

            size_t nextMatchLen = 0;
            if (findEmoteAt(line, lineLen, next, nextMatchLen, emoteSet, emoteCount) != nullptr)
                break;

            next += utf8CharLenClamped(line, next, lineLen);
        }

        if (next == i)
            next += utf8CharLenClamped(line, i, lineLen);

        cursorX = appendTextSpanAndMeasure(display, cursorX, fontY, fontHeight, line + i, next - i, true, fauxBold && inBold);
        i = next;
    }
}

} // namespace EmoteRenderer
} // namespace graphics

#endif // HAS_SCREEN
