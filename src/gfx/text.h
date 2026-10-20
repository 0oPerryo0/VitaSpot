#ifndef VAM_TEXT_H
#define VAM_TEXT_H

#include "gfx.h"

typedef enum font_weight { FW_REGULAR = 0, FW_BOLD = 1 } font_weight;

typedef enum text_align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } text_align;

/* Loads fonts (regular, bold, optional CJK fallback). Returns false if no
 * usable font was found; drawing then becomes a no-op. */
bool text_init(void);
void text_shutdown(void);
void text_begin_frame(void);
/* Strings rendered (cache misses) since text_begin_frame, for diagnostics. */
int text_renders_this_frame(void);

int text_line_height(int size);
/* Width of `s` laid out in one line, without rendering or caching it. Text
 * that needs the CJK fallback measures with the wrong glyphs until
 * text_fonts_ready(). */
int text_measure(const char *s, int size, font_weight w);
/* True once the font files are in memory and every script measures right. */
bool text_fonts_ready(void);
/* Where text_draw(s, x, y, ...) puts the body of the text, relative to y:
 * *top is the top of the tallest glyph, *bottom the baseline, or lower where
 * CJK characters reach below it (Latin descenders don't count). Text with CJK
 * sits lower, on the fallback font's ascent. False until the fonts can
 * measure s (see text_fonts_ready). */
bool text_vmetrics(const char *s, int size, font_weight w, float *top, float *bottom);
/* Width of `s`, after ellipsizing to max_w when max_w > 0. */
int text_width(const char *s, int size, font_weight w, float max_w);
/* Draws with the top of the line box at y. Returns drawn width. */
int text_draw(const char *s, float x, float y, int size, font_weight w, color c, float max_w);
int text_draw_aligned(const char *s, float x, float y, int size, font_weight w, color c,
                      float max_w, text_align align);
/* Vertically centred on cy. */
int text_draw_vc(const char *s, float x, float cy, int size, font_weight w, color c, float max_w);

/* Split `s` into up to max_lines lines no wider than max_w; the last line is
 * ellipsized. Draws them starting at y. Returns total height used. */
float text_draw_wrapped(const char *s, float x, float y, int size, font_weight w, color c,
                        float max_w, int max_lines);

#endif
