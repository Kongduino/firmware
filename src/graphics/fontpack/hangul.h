// Korean input with the 나랏글 (Naratgeul) keypad: 12 keys, as on Samsung phones. Plain C99, no allocation.
//
//   ㄱ  ㄴ  ㅏㅓ        1  2  3
//   ㄹ  ㅁ  ㅗㅜ        4  5  6
//   ㅅ  ㅇ  ㅣ          7  8  9
//   획추가 ㅡ 쌍자음    *  0  #
//
// 획추가 (add a stroke) changes the letter just typed: ㄱ→ㅋ→ㄱ, ㄴ→ㄷ→ㅌ→ㄴ, ㅁ→ㅂ→ㅍ→ㅁ, ㅅ→ㅈ→ㅊ→ㅅ, ㅇ→ㅎ→ㅇ, and
// ㅏ↔ㅑ, ㅓ↔ㅕ, ㅗ↔ㅛ, ㅜ↔ㅠ, ㅐ↔ㅒ, ㅔ↔ㅖ. 쌍자음 doubles it: ㄱ↔ㄲ, ㄷ↔ㄸ, ㅂ↔ㅃ, ㅅ↔ㅆ, ㅈ↔ㅉ. Pressed again, ㅏㅓ and ㅗㅜ
// switch their vowel; after ㅗ or ㅜ, ㅏㅓ gives the compound that exists (ㅘ, ㅝ); ㅣ makes ㅐ ㅔ ㅒ ㅖ ㅚ ㅟ ㅢ ㅙ ㅞ.
//
// Letters build syllables as on a phone: a final consonant moves to the next syllable when a vowel follows (갑 + ㅏ →
// 가바), and two finals make a compound one (ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ). A final that isn't valid yet, but may
// become so with 획추가 or 쌍자음 (ㄴ + ㅅ, on the way to ㄵ), is kept and shown after the syllable.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HG_KEY_G,      // ㄱ
    HG_KEY_N,      // ㄴ
    HG_KEY_AEO,    // ㅏ ㅓ
    HG_KEY_R,      // ㄹ
    HG_KEY_M,      // ㅁ
    HG_KEY_OU,     // ㅗ ㅜ
    HG_KEY_S,      // ㅅ
    HG_KEY_NG,     // ㅇ
    HG_KEY_I,      // ㅣ
    HG_KEY_STROKE, // 획추가
    HG_KEY_EU,     // ㅡ
    HG_KEY_DOUBLE, // 쌍자음
    HG_KEY_COUNT
} hg_key;

// Text a call gives back: finished, or still being composed. Code points: syllables (U+AC00..U+D7A3) and letters on
// their own (Hangul Compatibility Jamo, U+3131..U+3163)
#define HG_MAX_OUT 4

// The syllable being composed: its letters as Compatibility Jamo code points, 0 when absent
typedef struct {
    uint16_t initial, vowel, final1, final2;
    uint8_t last;      // the part typed last (what 획추가 and 쌍자음 change)
    uint8_t vowel_key; // the key that typed the vowel (to switch it when pressed again)
} hg_state;

void hg_init(hg_state *s);

// Applies a key: the code points it finished go to out, their count is returned
int hg_press(hg_state *s, hg_key key, uint32_t out[HG_MAX_OUT]);

// The text being composed: at most 2 code points (a syllable, and a final shown after it), their count
int hg_composing(const hg_state *s, uint32_t out[HG_MAX_OUT]);

// Removes the last letter of the text being composed; false when there's none (the caller deletes text instead)
bool hg_backspace(hg_state *s);

// Finishes the text being composed (before a space, punctuation, or leaving Korean): its code points, their count
int hg_flush(hg_state *s, uint32_t out[HG_MAX_OUT]);

// True while something is being composed
bool hg_composing_any(const hg_state *s);

#ifdef __cplusplus
}
#endif
