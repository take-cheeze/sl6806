/*
 * font5x7.h - the classic 5x7 bitmap font, ASCII 0x20..0x7E.
 *
 * Column-major: 5 bytes per glyph, one per column, bit 0 = top row. Glyphs
 * are drawn 6 columns wide so the extra column provides inter-character
 * spacing.
 */
#ifndef SL6806_FONT5X7_H
#define SL6806_FONT5X7_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL6806_FONT_FIRST  0x20
#define SL6806_FONT_LAST   0x7E
#define SL6806_FONT_WIDTH  5
#define SL6806_FONT_HEIGHT 7
#define SL6806_FONT_ADVANCE 6

extern const uint8_t sl6806_font5x7[(SL6806_FONT_LAST - SL6806_FONT_FIRST + 1)
                                    * SL6806_FONT_WIDTH];

#ifdef __cplusplus
}
#endif

#endif
