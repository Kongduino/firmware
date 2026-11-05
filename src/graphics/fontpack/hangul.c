// Korean input with the 나랏글 (Naratgeul) keypad: see hangul.h
#include "hangul.h"

#include <string.h>

enum { PART_NONE, PART_INITIAL, PART_VOWEL, PART_FINAL1, PART_FINAL2 };

// Compatibility Jamo letters
enum {
    G = 0x3131, GG = 0x3132, N = 0x3134, D = 0x3137, DD = 0x3138, R = 0x3139, M = 0x3141, B = 0x3142, BB = 0x3143,
    S = 0x3145, SS = 0x3146, NG = 0x3147, J = 0x3148, JJ = 0x3149, CH = 0x314A, K = 0x314B, T = 0x314C, P = 0x314D,
    H = 0x314E,
    A = 0x314F, AE = 0x3150, YA = 0x3151, YAE = 0x3152, EO = 0x3153, E = 0x3154, YEO = 0x3155, YE = 0x3156, O = 0x3157,
    WA = 0x3158, WAE = 0x3159, OE = 0x315A, YO = 0x315B, U = 0x315C, WO = 0x315D, WE = 0x315E, WI = 0x315F, YU = 0x3160,
    EU = 0x3161, UI = 0x3162, I = 0x3163
};

// Syllable composition orders (Unicode 3.12)
static const uint16_t initials[19] = {G, GG, N, D, DD, R, M, B, BB, S, SS, NG, J, JJ, CH, K, T, P, H};
static const uint16_t finals[27] = {G,      GG,     0x3133, N,      0x3135, 0x3136, D, R, 0x313A,
                                    0x313B, 0x313C, 0x313D, 0x313E, 0x313F, 0x3140, M, B, 0x3144,
                                    S,      SS,     NG,     J,      CH,     K,      T, P, H};

// Two finals that make one: first, second, compound
static const uint16_t compounds[][3] = {{G, S, 0x3133}, {N, J, 0x3135}, {N, H, 0x3136}, {R, G, 0x313A},
                                        {R, M, 0x313B}, {R, B, 0x313C}, {R, S, 0x313D}, {R, T, 0x313E},
                                        {R, P, 0x313F}, {R, H, 0x3140}, {B, S, 0x3144}};

// Pairs: a letter and what a key makes of it
static const uint16_t stroke_map[][2] = {{G, K},   {K, G},   {N, D},    {D, T},   {T, N},   {M, B},   {B, P},
                                         {P, M},   {S, J},   {J, CH},   {CH, S},  {NG, H},  {H, NG},  {A, YA},
                                         {YA, A},  {EO, YEO}, {YEO, EO}, {O, YO},  {YO, O},  {U, YU},  {YU, U},
                                         {AE, YAE}, {YAE, AE}, {E, YE},  {YE, E}};
static const uint16_t double_map[][2] = {{G, GG}, {GG, G}, {D, DD}, {DD, D}, {B, BB},
                                         {BB, B}, {S, SS}, {SS, S}, {J, JJ}, {JJ, J}};
// A vowel followed by ㅣ
static const uint16_t with_i[][2] = {{A, AE}, {YA, YAE}, {EO, E}, {YEO, YE}, {O, OE},
                                     {WA, WAE}, {U, WI}, {WO, WE}, {EU, UI}};
// The vowel a compound one was made from (backspace)
static const uint16_t vowel_parts[][2] = {{AE, A}, {YAE, YA}, {E, EO}, {YE, YEO}, {OE, O},  {WAE, WA},
                                          {WI, U}, {WE, WO},  {UI, EU}, {WA, O},  {WO, U}};

static const uint16_t key_letters[HG_KEY_COUNT] = {G, N, A, R, M, O, S, NG, I, 0, EU, 0};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static uint16_t lookup(const uint16_t (*map)[2], int n, uint16_t from)
{
    for (int i = 0; i < n; i++)
        if (map[i][0] == from)
            return map[i][1];
    return 0;
}

static int index_of(const uint16_t *list, int n, uint16_t cp)
{
    for (int i = 0; i < n; i++)
        if (list[i] == cp)
            return i;
    return -1;
}

static uint16_t compound(uint16_t first, uint16_t second)
{
    for (int i = 0; i < COUNT(compounds); i++)
        if (compounds[i][0] == first && compounds[i][1] == second)
            return compounds[i][2];
    return 0;
}

// True when a consonant, or one 획추가 and 쌍자음 make of it, follows first in a compound final
static bool may_follow(uint16_t first, uint16_t c)
{
    uint16_t seen[8] = {c};
    int n = 1;
    for (int i = 0; i < n; i++) {
        if (compound(first, seen[i]))
            return true;
        const uint16_t next[2] = {lookup(stroke_map, COUNT(stroke_map), seen[i]),
                                  lookup(double_map, COUNT(double_map), seen[i])};
        for (int k = 0; k < 2; k++)
            if (next[k] && n < COUNT(seen) && index_of(seen, n, next[k]) < 0)
                seen[n++] = next[k];
    }
    return false;
}

static uint32_t syllable(uint16_t initial, uint16_t vowel, uint16_t final)
{
    const int l = index_of(initials, COUNT(initials), initial);
    const int t = final ? index_of(finals, COUNT(finals), final) + 1 : 0;
    return 0xAC00 + (uint32_t)((l * 21 + (vowel - A)) * 28 + t);
}

void hg_init(hg_state *s)
{
    memset(s, 0, sizeof(*s));
}

bool hg_composing_any(const hg_state *s)
{
    return s->initial || s->vowel;
}

int hg_composing(const hg_state *s, uint32_t out[HG_MAX_OUT])
{
    if (!s->vowel) {
        if (!s->initial)
            return 0;
        out[0] = s->initial;
        return 1;
    }
    if (!s->initial) {
        out[0] = s->vowel;
        return 1;
    }
    // A final that isn't one (yet) is shown after the syllable
    const uint16_t both = s->final2 ? compound(s->final1, s->final2) : 0;
    if (both) {
        out[0] = syllable(s->initial, s->vowel, both);
        return 1;
    }
    const bool final1_ok = s->final1 && index_of(finals, COUNT(finals), s->final1) >= 0;
    out[0] = syllable(s->initial, s->vowel, final1_ok ? s->final1 : 0);
    int n = 1;
    if (s->final1 && !final1_ok)
        out[n++] = s->final1;
    if (s->final2)
        out[n++] = s->final2;
    return n;
}

int hg_flush(hg_state *s, uint32_t out[HG_MAX_OUT])
{
    const int n = hg_composing(s, out);
    hg_init(s);
    return n;
}

// Finishes the syllable without its finals (they move to the next one): the syllable's code points, their count
static int finish_before_finals(hg_state *s, uint32_t out[HG_MAX_OUT])
{
    out[0] = syllable(s->initial, s->vowel, 0);
    return 1;
}

static int press_vowel(hg_state *s, hg_key key, uint32_t out[HG_MAX_OUT])
{
    const uint16_t letter = key_letters[key];
    if (s->vowel && !s->final1) {
        // The same key again switches its vowel (ㅏ↔ㅓ, ㅗ↔ㅜ); after ㅘ or ㅝ it does nothing
        if (s->last == PART_VOWEL && s->vowel_key == key) {
            static const uint16_t again[][2] = {{A, EO}, {EO, A}, {YA, YEO}, {YEO, YA}, {O, U}, {U, O}, {YO, YU}, {YU, YO}};
            const uint16_t next = lookup(again, COUNT(again), s->vowel);
            if (next) {
                s->vowel = next;
                return 0;
            }
            if (key == HG_KEY_AEO && (s->vowel == WA || s->vowel == WO))
                return 0;
        }
        // A compound vowel
        uint16_t made = 0;
        if (key == HG_KEY_AEO)
            made = s->vowel == O ? WA : s->vowel == U ? WO : 0;
        else if (key == HG_KEY_I)
            made = lookup(with_i, COUNT(with_i), s->vowel);
        if (made) {
            s->vowel = made;
            s->last = PART_VOWEL;
            s->vowel_key = (uint8_t)key;
            return 0;
        }
    }

    int n = 0;
    uint16_t initial = 0;
    if (s->vowel && s->initial && s->final2) {
        // The last final starts the next syllable
        out[0] = syllable(s->initial, s->vowel, s->final1);
        n = 1;
        initial = s->final2;
    } else if (s->vowel && s->initial && s->final1) {
        n = finish_before_finals(s, out);
        initial = s->final1;
    } else if (s->vowel) {
        n = hg_composing(s, out);
    } else {
        initial = s->initial;
    }
    hg_init(s);
    s->initial = initial;
    s->vowel = letter;
    s->last = PART_VOWEL;
    s->vowel_key = (uint8_t)key;
    return n;
}

static int press_consonant(hg_state *s, uint16_t letter, uint32_t out[HG_MAX_OUT])
{
    if (s->initial && s->vowel) {
        if (!s->final1) {
            s->final1 = letter;
            s->last = PART_FINAL1;
            return 0;
        }
        if (!s->final2 && index_of(finals, COUNT(finals), s->final1) >= 0 && may_follow(s->final1, letter)) {
            s->final2 = letter;
            s->last = PART_FINAL2;
            return 0;
        }
    }
    // A new syllable
    const int n = hg_flush(s, out);
    s->initial = letter;
    s->last = PART_INITIAL;
    return n;
}

// 획추가 or 쌍자음: changes the letter typed last
static void modify(hg_state *s, const uint16_t (*map)[2], int count)
{
    uint16_t *part = s->last == PART_INITIAL  ? &s->initial
                     : s->last == PART_VOWEL  ? &s->vowel
                     : s->last == PART_FINAL1 ? &s->final1
                     : s->last == PART_FINAL2 ? &s->final2
                                              : NULL;
    if (!part || !*part)
        return;
    const uint16_t next = lookup(map, count, *part);
    if (next)
        *part = next;
}

int hg_press(hg_state *s, hg_key key, uint32_t out[HG_MAX_OUT])
{
    switch (key) {
    case HG_KEY_STROKE:
        modify(s, stroke_map, COUNT(stroke_map));
        return 0;
    case HG_KEY_DOUBLE:
        modify(s, double_map, COUNT(double_map));
        return 0;
    case HG_KEY_AEO:
    case HG_KEY_OU:
    case HG_KEY_I:
    case HG_KEY_EU:
        return press_vowel(s, key, out);
    default:
        if ((unsigned)key >= HG_KEY_COUNT)
            return 0;
        return press_consonant(s, key_letters[key], out);
    }
}

bool hg_backspace(hg_state *s)
{
    if (s->final2) {
        s->final2 = 0;
        s->last = PART_FINAL1;
    } else if (s->final1) {
        s->final1 = 0;
        s->last = PART_VOWEL;
    } else if (s->vowel) {
        const uint16_t part = lookup(vowel_parts, COUNT(vowel_parts), s->vowel);
        s->vowel = part;
        if (part) {
            s->last = PART_VOWEL;
            s->vowel_key = (uint8_t)(part == A || part == EO || part == YA || part == YEO ? HG_KEY_AEO
                                     : part == O || part == U || part == YO || part == YU ? HG_KEY_OU
                                     : part == EU                                       ? HG_KEY_EU
                                                                                        : HG_KEY_I);
        } else {
            s->last = s->initial ? PART_INITIAL : PART_NONE;
        }
    } else if (s->initial) {
        s->initial = 0;
        s->last = PART_NONE;
    } else {
        return false;
    }
    return true;
}
