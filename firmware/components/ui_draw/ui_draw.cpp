#include "ui_draw.h"

#include "display_bsp.h"

/* 屏幕实例在 main.cpp 里定义 */
extern DisplayPort RlcdPort;

/* 逻辑色 -> 底层色。RLCD_SetPixel 只判断"非零即亮"。 */
#define TO_LCD(dark)   ((dark) ? ColorWhite : ColorBlack)

/* ============================================================
 *  基础图元
 * ============================================================ */
void Ui_Pixel(int x, int y, int dark)
{
    RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, TO_LCD(dark));
}

void Ui_Clear(int dark)
{
    RlcdPort.RLCD_ColorClear(TO_LCD(dark));
}

void Ui_FillRect(int x, int y, int w, int h, int dark)
{
    uint8_t c = TO_LCD(dark);
    for (int r = 0; r < h; r++) {
        for (int col = 0; col < w; col++) {
            RlcdPort.RLCD_SetPixel((uint16_t)(x + col), (uint16_t)(y + r), c);
        }
    }
}

void Ui_HLine(int x, int y, int w, int dark)
{
    uint8_t c = TO_LCD(dark);
    for (int i = 0; i < w; i++) {
        RlcdPort.RLCD_SetPixel((uint16_t)(x + i), (uint16_t)y, c);
    }
}

void Ui_VLine(int x, int y, int h, int dark)
{
    uint8_t c = TO_LCD(dark);
    for (int i = 0; i < h; i++) {
        RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)(y + i), c);
    }
}

/* ============================================================
 *  文字
 * ============================================================ */
/* 表是按 code 升序生成的,二分查找 */
static const UiGlyph *FindGlyph(const UiFont *f, uint32_t code)
{
    int lo = 0;
    int hi = (int)f->cnt - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = f->glyphs[mid].code;
        if (c == code) {
            return &f->glyphs[mid];
        }
        if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

/* 解一个 UTF-8 字符,返回码点(0 = 字符串结束),指针前移 */
static uint32_t NextUtf8(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t c = p[0];

    if (c == 0) {
        return 0;
    }
    if (c < 0x80) {
        *pp += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && p[1] != 0) {
        *pp += 2;
        return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    }
    if ((c & 0xF0) == 0xE0 && p[1] != 0 && p[2] != 0) {
        *pp += 3;
        return ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && p[1] != 0 && p[2] != 0 && p[3] != 0) {
        *pp += 4;
        return ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
               ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
    }

    *pp += 1;                       /* 非法字节,当占位符处理 */
    return '?';
}

int Ui_Text(int x, int baseline_y, const UiFont *font,
            const char *utf8, int dark)
{
    int pen = x;
    const char *p = utf8;

    for (;;) {
        uint32_t code = NextUtf8(&p);
        if (code == 0) {
            break;
        }
        if (code == '\n') {
            pen = x;
            baseline_y += font->line_h;
            continue;
        }

        const UiGlyph *g = FindGlyph(font, code);
        if (g == nullptr) {
            /* 字体里没这个字。画个小方块让它**看得见** ——
               悄悄留白的话,以后改文案漏补字根本发现不了。 */
            Ui_FillRect(pen + 1, baseline_y - font->base + 3,
                        (font->line_h / 2), (font->line_h / 2), dark);
            pen += (font->line_h / 2) + 2;
            continue;
        }

        if (g->w > 0) {
            int row_bytes = (g->w + 7) / 8;
            for (int r = 0; r < g->h; r++) {
                const uint8_t *row = font->bits + g->off + r * row_bytes;
                int yy = baseline_y + g->oy + r;
                for (int c = 0; c < g->w; c++) {
                    if (row[c >> 3] & (0x80 >> (c & 7))) {
                        Ui_Pixel(pen + g->ox + c, yy, dark);
                    }
                }
            }
        }
        pen += g->adv;
    }
    return pen;
}

int Ui_TextWidth(const UiFont *font, const char *utf8)
{
    int widest = 0;
    int line   = 0;
    const char *p = utf8;

    for (;;) {
        uint32_t code = NextUtf8(&p);
        if (code == 0) {
            break;
        }
        if (code == '\n') {
            if (line > widest) {
                widest = line;
            }
            line = 0;
            continue;
        }
        const UiGlyph *g = FindGlyph(font, code);
        line += (g != nullptr) ? g->adv : (font->line_h / 2 + 2);
    }
    return (line > widest) ? line : widest;
}

void Ui_TextCentered(int cx, int baseline_y, const UiFont *font,
                     const char *utf8, int dark)
{
    /* 多行时每行分别居中,所以要自己走一遍 */
    const char *p = utf8;
    const char *line = p;

    for (;;) {
        uint32_t code = NextUtf8(&p);
        if (code == '\n' || code == 0) {
            int w = Ui_TextWidth(font, line);
            /* Ui_TextWidth 对整段会取最宽行,这里只想量当前行,
               所以临时截断:用一个小缓冲不划算,直接算一遍更省事 */
            w = 0;
            const char *q = line;
            while (q < p - 1 || (code == 0 && q < p)) {
                uint32_t c2 = NextUtf8(&q);
                if (c2 == 0 || c2 == '\n') {
                    break;
                }
                const UiGlyph *g = FindGlyph(font, c2);
                w += (g != nullptr) ? g->adv : (font->line_h / 2 + 2);
            }

            Ui_Text(cx - w / 2, baseline_y, font, line, dark);

            if (code == 0) {
                break;
            }
            baseline_y += font->line_h;
            line = p;
        }
    }
}
