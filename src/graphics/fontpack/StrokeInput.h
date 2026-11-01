#pragma once

// Typing Chinese by strokes (五笔画): the user enters the first strokes of a character, 横 竖 撇 点 折, and picks it
// from the candidates of a stroke dictionary (.msd, see https://github.com/Kongduino/MeshtasticFontPacks STROKES.md),
// installed as kDictPath. The characters are drawn from the font packs.

#include "graphics/fontpack/FontPacks.h"

#if HAS_FONTPACKS

#include <stdint.h>

namespace graphics
{
namespace StrokeInput
{

/// Where the stroke dictionary is installed (uploaded like a font pack, by XModem)
constexpr const char *kDictPath = "/fonts/strokes.msd";

/// Candidates shown per page
constexpr int kPageSize = 9;

/// The wildcard stroke: any one stroke
constexpr uint8_t kAnyStroke = 6;

/// True when a valid dictionary is installed. The first call opens it and checks its CRC
bool available();

/// Closes the dictionary (before it's replaced or removed); the next use opens it again
void close();

/// Clears the strokes typed so far
void reset();

/// Adds a stroke (1 to 5, or kAnyStroke) and looks the candidates up; false when there's no room for more
bool addStroke(uint8_t stroke);

/// Removes the last stroke; false when there's none
bool removeStroke();

/// The strokes typed so far
int strokeCount();
uint8_t strokeAt(int i);

/// The candidates of the current page (up to kPageSize), best first
int pageCandidateCount();
uint32_t pageCandidate(int i);

/// Pages through the candidates (up to 64 of them)
int page();
int pageCount();
void nextPage();
void prevPage();

/// The candidate at index i of the current page, the strokes then cleared; 0 when there's none
uint32_t pick(int i);

} // namespace StrokeInput
} // namespace graphics

#endif
