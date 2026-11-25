// Word wrapping for the message screen (MessageRenderer::generateLines). Every wrapped line must fit
// the width it was wrapped to, and a break must never land inside a UTF-8 character or an emote.
// A word wider than the line used to be placed whole (including the character that overflowed),
// then split into alternating 1-char and overwide lines.

#include "TestUtil.h"
#include "configuration.h"
#include <unity.h>

#if HAS_SCREEN

#include "graphics/EmoteRenderer.h"
#include "graphics/draw/MessageRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayFonts.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using graphics::MessageRenderer::generateLines;

// Headless display with a real font, so widths come from the production measuring path.
class FakeDisplay : public OLEDDisplay
{
  public:
    FakeDisplay() { setFont(ArialMT_Plain_10); }
    void display() override {}
    int getBufferOffset() override { return 0; }
    size_t write(uint8_t) override { return 1; }
};

static int widthOf(FakeDisplay &d, const std::string &s)
{
    return graphics::UIRenderer::measureStringWithEmotes(&d, s);
}

static std::string joined(const std::vector<std::string> &lines)
{
    std::string all;
    for (const auto &l : lines)
        all += l;
    return all;
}

static void assertAllFit(FakeDisplay &d, const std::vector<std::string> &lines, int width)
{
    for (const auto &l : lines) {
        char msg[96];
        snprintf(msg, sizeof(msg), "line \"%s\" is %d px, limit %d", l.c_str(), widthOf(d, l), width);
        TEST_ASSERT_TRUE_MESSAGE(widthOf(d, l) <= width, msg);
    }
}

void setUp(void) {}
void tearDown(void) {}

void test_generateLines_longWord_splitsWithinWidth(void)
{
    FakeDisplay d;
    const char *word = "supercalifragilisticexpialidocious";
    const int width = 40;
    TEST_ASSERT_TRUE(widthOf(d, word) > 2 * width); // needs at least three lines

    auto lines = generateLines(&d, "", word, width);
    TEST_ASSERT_TRUE(lines.size() >= 3);
    assertAllFit(d, lines, width);
    TEST_ASSERT_EQUAL_STRING(word, joined(lines).c_str());

    // Every line except the last is filled: one more character would not have fit.
    for (size_t i = 0; i + 1 < lines.size(); ++i)
        TEST_ASSERT_TRUE(widthOf(d, lines[i] + lines[i + 1][0]) > width);
}

void test_generateLines_longWord_startsOnFreshLine(void)
{
    FakeDisplay d;
    auto lines = generateLines(&d, "", "hi supercalifragilisticexpialidocious", 40);
    TEST_ASSERT_TRUE(lines.size() >= 3);
    TEST_ASSERT_EQUAL_STRING("hi", lines[0].c_str());
    assertAllFit(d, lines, 40);
    TEST_ASSERT_EQUAL_STRING("hisupercalifragilisticexpialidocious", joined(lines).c_str());
}

void test_generateLines_fittingWords_breakOnlyAtSpaces(void)
{
    FakeDisplay d;
    const char *text = "the quick brown fox jumps over the lazy dog";
    auto lines = generateLines(&d, "", text, 50);
    TEST_ASSERT_TRUE(lines.size() > 1);
    assertAllFit(d, lines, 50);
    // Breaks fall on spaces, and the space at a break is dropped rather than left trailing.
    std::string rejoined;
    for (const auto &l : lines) {
        TEST_ASSERT_NOT_EQUAL(' ', l.back());
        rejoined += (rejoined.empty() ? "" : " ") + l;
    }
    TEST_ASSERT_EQUAL_STRING(text, rejoined.c_str());
}

void test_generateLines_trailingSpace_notCounted(void)
{
    // "over the" fits but "over the " does not: the line must not be judged (or drawn) with the space.
    FakeDisplay d;
    const int width = widthOf(d, "over the");
    TEST_ASSERT_TRUE(widthOf(d, "over the ") > width);
    auto lines = generateLines(&d, "", "over the lazy dog", width);
    TEST_ASSERT_TRUE(lines.size() >= 2);
    TEST_ASSERT_EQUAL_STRING("over the", lines[0].c_str());
    assertAllFit(d, lines, width);
}

void test_generateLines_multibyte_neverSplitsChar(void)
{
    FakeDisplay d;
    std::string text;
    for (int i = 0; i < 40; ++i)
        text += "a\xC3\xA9"; // "aé"
    auto lines = generateLines(&d, "", text.c_str(), 30);
    TEST_ASSERT_TRUE(lines.size() > 1);
    assertAllFit(d, lines, 30);
    TEST_ASSERT_EQUAL_STRING(text.c_str(), joined(lines).c_str());
    for (const auto &l : lines) {
        TEST_ASSERT_FALSE((uint8_t)l.front() == 0xA9); // starts on a continuation byte
        TEST_ASSERT_FALSE((uint8_t)l.back() == 0xC3);  // ends on a lead byte
    }
}

void test_generateLines_emotes_neverSplit(void)
{
    FakeDisplay d;
    const std::string thumb = "\xF0\x9F\x91\x8D"; // U+1F44D, a supported emote
    std::string text;
    for (int i = 0; i < 20; ++i)
        text += thumb;
    TEST_ASSERT_TRUE(widthOf(d, thumb) > 0);
    const int width = widthOf(d, thumb) * 3;

    auto lines = generateLines(&d, "", text.c_str(), width);
    TEST_ASSERT_TRUE(lines.size() > 1);
    assertAllFit(d, lines, width);
    TEST_ASSERT_EQUAL_STRING(text.c_str(), joined(lines).c_str());
    for (const auto &l : lines)
        TEST_ASSERT_EQUAL_size_t(0, l.size() % thumb.size());
}

void test_generateLines_newlines_preserved(void)
{
    FakeDisplay d;
    auto lines = generateLines(&d, "", "one\ntwo\n\nthree", 100);
    TEST_ASSERT_EQUAL_size_t(3, lines.size());
    TEST_ASSERT_EQUAL_STRING("one", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("two", lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("three", lines[2].c_str());
}

void test_generateLines_curlyApostrophe_becomesPlain(void)
{
    FakeDisplay d;
    auto lines = generateLines(&d, "", "don\xE2\x80\x99t", 100);
    TEST_ASSERT_EQUAL_size_t(1, lines.size());
    TEST_ASSERT_EQUAL_STRING("don't", lines[0].c_str());
}

void test_generateLines_truncatedLead_noOverread(void)
{
    // A truncated multi-byte lead at the very end of an exact-sized buffer: any read past the
    // content is a heap-buffer-overflow under the coverage env's AddressSanitizer.
    FakeDisplay d;
    static const char content[] = "abc\xF0";
    char *buf = (char *)malloc(sizeof(content));
    memcpy(buf, content, sizeof(content));
    auto lines = generateLines(&d, "", buf, 100);
    free(buf);
    TEST_ASSERT_EQUAL_size_t(1, lines.size());
    TEST_ASSERT_EQUAL_STRING(content, lines[0].c_str());
}

// --- Line breaks in scripts written without spaces (CJK), with the CJK line-breaking rules (kinsoku) ---

using graphics::EmoteRenderer::canBreakBetween;

void test_canBreakBetween_cjk_characters(void)
{
    TEST_ASSERT_TRUE(canBreakBetween(0x65E5, 0x672C)); // 日|本
    TEST_ASSERT_TRUE(canBreakBetween(0xAC00, 0xAC01)); // 가|각 (Hangul)
    TEST_ASSERT_TRUE(canBreakBetween('a', 0x65E5));    // a|日
    TEST_ASSERT_TRUE(canBreakBetween(0x65E5, 'a'));    // 日|a
    TEST_ASSERT_TRUE(canBreakBetween(0x65E5, 0x300C)); // 日|「
    TEST_ASSERT_FALSE(canBreakBetween('a', 'b'));      // Latin: only at spaces
    TEST_ASSERT_FALSE(canBreakBetween(0, 0x65E5));     // nothing before
}

void test_canBreakBetween_noBreakBeforeClosingAndPunctuation(void)
{
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0x3002)); // 日。
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0x3001)); // 日、
    TEST_ASSERT_FALSE(canBreakBetween(0x4E2D, 0xFF0C)); // 中，(Chinese fullwidth comma)
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0x300D)); // 日」
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0xFF09)); // 日）
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, '.'));
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, ','));
    TEST_ASSERT_FALSE(canBreakBetween(0x304B, 0x3063)); // か っ (small kana)
    TEST_ASSERT_FALSE(canBreakBetween(0x30AB, 0x30FC)); // カ ー (long-vowel mark)
    TEST_ASSERT_FALSE(canBreakBetween(0xFF76, 0xFF6F)); // ｶ ｯ (halfwidth small kana)
}

void test_canBreakBetween_noBreakAfterOpening(void)
{
    TEST_ASSERT_FALSE(canBreakBetween(0x300C, 0x65E5)); // 「日
    TEST_ASSERT_FALSE(canBreakBetween(0xFF08, 0x65E5)); // （日
    TEST_ASSERT_FALSE(canBreakBetween('(', 0x65E5));    // (日
}

void test_canBreakBetween_fullwidthAlnumRunsStayTogether(void)
{
    TEST_ASSERT_FALSE(canBreakBetween(0xFF11, 0xFF12)); // １２
    TEST_ASSERT_FALSE(canBreakBetween(0xFF21, 0xFF42)); // Ａｂ
    TEST_ASSERT_TRUE(canBreakBetween(0x65E5, 0xFF11));  // 日１
    TEST_ASSERT_TRUE(canBreakBetween(0xFF11, 0x65E5));  // １日
}

void test_canBreakBetween_modifiersStayWithTheirCharacter(void)
{
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0xFE0F));  // variation selector 16
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0xFE0E));  // variation selector 15
    TEST_ASSERT_FALSE(canBreakBetween(0x65E5, 0x1F3FB)); // skin tone
}

static bool startsWith(const std::string &s, const char *prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

static bool endsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Wrapped at every width from three characters to wide: no line may start with closing punctuation, a small kana or a
// variation selector, and none may end with an opening bracket. Narrower than the longest group the rules keep together
// (ちょっ, three characters), a group that overflows a line on its own is still cut.
void test_generateLines_cjk_kinsokuAtEveryWidth(void)
{
    FakeDisplay d;
    const int narrowest = widthOf(d, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"); // 日本語
    const char *text =
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x80\x8C\xE6\x96\x87\xE7\xAB\xA0\xE3\x80\x8D" // 日本語の「文章」
        "\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82\xE3\x81\xA1\xE3\x82\x87\xE3\x81\xA3\xE3\x81\xA8"             // です。ちょっと
        "\xE5\xBE\x85\xE3\x81\xA3\xE3\x81\xA6\xE3\x80\x81\xE6\x98\x9F\xEF\xB8\x8F\xE3\x81\x8C"             // 待って、星️が
        "\xE8\xA6\x8B\xE3\x81\x88\xE3\x82\x8B\xEF\xBC\x81";                                                // 見える！
    for (int width = narrowest; width <= narrowest + 160; width += 3) {
        auto lines = generateLines(&d, "", text, width);
        TEST_ASSERT_EQUAL_STRING(text, joined(lines).c_str());
        for (size_t i = 1; i < lines.size(); ++i) {
            char msg[96];
            snprintf(msg, sizeof(msg), "width %d, line %u", width, (unsigned)i);
            const std::string &l = lines[i];
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xE3\x80\x82"), msg); // 。
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xE3\x80\x81"), msg); // 、
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xE3\x80\x8D"), msg); // 」
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xE3\x81\xA3"), msg); // っ
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xE3\x82\x87"), msg); // ょ
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xEF\xBC\x81"), msg); // ！
            TEST_ASSERT_FALSE_MESSAGE(startsWith(l, "\xEF\xB8\x8F"), msg); // VS16
        }
        for (size_t i = 0; i + 1 < lines.size(); ++i)
            TEST_ASSERT_FALSE(endsWith(lines[i], "\xE3\x80\x8C")); // 「
    }
}

// A run of CJK wraps between its characters, not only where a character overflows the line.
void test_generateLines_cjk_breaksBetweenCharacters(void)
{
    FakeDisplay d;
    const char *text = "hello \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // hello 日本語日本語
    const int width = widthOf(d, "hello \xE6\x97\xA5");
    auto lines = generateLines(&d, "", text, width);
    TEST_ASSERT_TRUE(lines.size() >= 2);
    TEST_ASSERT_EQUAL_STRING("hello \xE6\x97\xA5", lines[0].c_str()); // the first character fills the first line
    assertAllFit(d, lines, width);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_generateLines_longWord_splitsWithinWidth);
    RUN_TEST(test_generateLines_longWord_startsOnFreshLine);
    RUN_TEST(test_generateLines_fittingWords_breakOnlyAtSpaces);
    RUN_TEST(test_generateLines_trailingSpace_notCounted);
    RUN_TEST(test_generateLines_multibyte_neverSplitsChar);
    RUN_TEST(test_generateLines_emotes_neverSplit);
    RUN_TEST(test_generateLines_newlines_preserved);
    RUN_TEST(test_generateLines_curlyApostrophe_becomesPlain);
    RUN_TEST(test_generateLines_truncatedLead_noOverread);
    RUN_TEST(test_canBreakBetween_cjk_characters);
    RUN_TEST(test_canBreakBetween_noBreakBeforeClosingAndPunctuation);
    RUN_TEST(test_canBreakBetween_noBreakAfterOpening);
    RUN_TEST(test_canBreakBetween_fullwidthAlnumRunsStayTogether);
    RUN_TEST(test_canBreakBetween_modifiersStayWithTheirCharacter);
    RUN_TEST(test_generateLines_cjk_kinsokuAtEveryWidth);
    RUN_TEST(test_generateLines_cjk_breaksBetweenCharacters);
    exit(UNITY_END());
}

void loop() {}

#else // !HAS_SCREEN

void setUp(void) {}
void tearDown(void) {}
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}
void loop() {}

#endif
