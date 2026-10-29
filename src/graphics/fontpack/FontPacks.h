#pragma once

#include "configuration.h"

// Font packs: glyphs for the code points the built-in fonts don't have (CJK, ...), installed at run time as .mfp files
// (see https://github.com/Kongduino/MeshtasticFontPacks). Set MESHTASTIC_EXCLUDE_FONTPACKS to leave them out.
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_FONTPACKS
#include "FSCommon.h"
#ifdef FSCom
#define HAS_FONTPACKS 1
#endif
#endif
#ifndef HAS_FONTPACKS
#define HAS_FONTPACKS 0
#endif

#if HAS_FONTPACKS

#include <OLEDDisplay.h>
#include <stdint.h>

namespace graphics
{
namespace FontPacks
{

/// Where the packs are installed
constexpr const char *kFontDir = "/fonts";

/// True when at least one valid pack is installed. The first call loads the packs from kFontDir.
bool available();

/// Drops the loaded packs and loads them again (after a pack was installed or removed). Screen thread only.
void reload();

/// Asks for a reload at the next use of the packs, from any thread (the file transfer, when a pack arrives)
void requestReload();

/// True for a path where a pack is installed: kFontDir/<name>.mfp
bool isPackPath(const char *path);

/// Creates kFontDir when it doesn't exist; the caller holds the SPI lock
void ensureDirLocked();

/// True when the built-in fonts draw this code point themselves (ASCII, Latin-1, and the script of a language build
/// such as OLED_RU); the packs are only used for the others
bool builtinCovers(uint32_t cp);

/// True when a code point is drawn from the packs: packs are installed and the built-in fonts don't have it
inline bool handles(uint32_t cp)
{
    return !builtinCovers(cp) && available();
}

/// The pixels a code point takes on the line: its glyph's advance, or the width of the box drawn when no pack has it
int advance(uint32_t cp);

/// Draws a code point on a line of text whose top is y and height fontHeight (a box when no pack has it).
/// Returns the advance.
int draw(OLEDDisplay *display, int x, int y, int fontHeight, uint32_t cp);

/// The line height of the packs (the tallest), 0 when none is installed
int lineHeight();

/// Decodes the UTF-8 character at s (at most len bytes): its code point, and its length in charLen. An invalid
/// sequence gives U+FFFD and a length of 1.
uint32_t decodeUtf8(const char *s, size_t len, size_t &charLen);

} // namespace FontPacks
} // namespace graphics

#endif
