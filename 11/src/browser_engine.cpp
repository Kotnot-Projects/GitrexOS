#include "browser_engine.hpp"

namespace gitrex {

static char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with(const char* text, const char* prefix) {
    while (*prefix) {
        if (!*text || ascii_lower(*text++) != ascii_lower(*prefix++)) return false;
    }
    return true;
}

static bool contains(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return false;
    for (const char* p = text; *p; ++p) {
        if (starts_with(p, needle)) return true;
    }
    return false;
}

static const char* find_text(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return 0;
    for (const char* p = text; *p; ++p) {
        if (starts_with(p, needle)) return p;
    }
    return 0;
}

bool browser_contains(const char* text, const char* needle) {
    return contains(text, needle);
}

static int hex_value(char c) {
    c = ascii_lower(c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool color_token(const char* value, unsigned char* result) {
    static const unsigned char ega[16][3] = {
        {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
        {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
        {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
        {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255}
    };
    unsigned char red, green, blue;
    unsigned int best_distance = 0xFFFFFFFFu;
    unsigned int i;
    int length = 0;
    if (*value == '#') {
        int digits[6];
        ++value;
        while (length < 6 && value[length]) {
            digits[length] = hex_value(value[length]);
            if (digits[length] < 0) break;
            ++length;
        }
        if (length == 3) {
            red = (unsigned char)(digits[0] * 17);
            green = (unsigned char)(digits[1] * 17);
            blue = (unsigned char)(digits[2] * 17);
        } else if (length == 6) {
            red = (unsigned char)(digits[0] * 16 + digits[1]);
            green = (unsigned char)(digits[2] * 16 + digits[3]);
            blue = (unsigned char)(digits[4] * 16 + digits[5]);
        } else {
            return false;
        }
    } else {
        while (value[length] && value[length] != ';' && value[length] != '}' &&
               value[length] != '"' && value[length] != '\'') ++length;
        while (length && value[length - 1] == ' ') --length;
        if (length == 3 && starts_with(value, "red")) { *result = 4; return true; }
        if (length == 4 && starts_with(value, "blue")) { *result = 1; return true; }
        if (length == 5 && starts_with(value, "green")) { *result = 2; return true; }
        if (length == 4 && starts_with(value, "cyan")) { *result = 3; return true; }
        if (length == 7 && starts_with(value, "magenta")) { *result = 5; return true; }
        if (length == 5 && starts_with(value, "brown")) { *result = 6; return true; }
        if (length == 9 && starts_with(value, "lightgray")) { *result = 7; return true; }
        if (length == 8 && starts_with(value, "darkgray")) { *result = 8; return true; }
        if (length == 9 && starts_with(value, "lightblue")) { *result = 9; return true; }
        if (length == 10 && starts_with(value, "lightgreen")) { *result = 10; return true; }
        if (length == 9 && starts_with(value, "lightcyan")) { *result = 11; return true; }
        if (length == 8 && starts_with(value, "yellow")) { *result = 14; return true; }
        if (length == 5 && starts_with(value, "white")) { *result = 15; return true; }
        if (length == 5 && starts_with(value, "black")) { *result = 0; return true; }
        return false;
    }

    for (i = 0; i < 16; ++i) {
        int dr = (int)red - ega[i][0];
        int dg = (int)green - ega[i][1];
        int db = (int)blue - ega[i][2];
        unsigned int distance = (unsigned int)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            *result = (unsigned char)i;
        }
    }
    return true;
}

static unsigned char css_color(const char* html, const char* property,
                               unsigned char fallback) {
    const char* p = html;
    unsigned char result;
    while (p && *p) {
        if (starts_with(p, property) &&
            (p == html || ((p[-1] < 'a' || p[-1] > 'z') &&
                           (p[-1] < 'A' || p[-1] > 'Z') &&
                           (p[-1] < '0' || p[-1] > '9') && p[-1] != '-'))) {
            const char* value = p;
            while (*value && *value != ':') ++value;
            if (*value == ':') {
                ++value;
                while (*value == ' ' || *value == '\t') ++value;
                if (color_token(value, &result)) return result;
            }
        }
        ++p;
    }
    return fallback;
}

BrowserStyle browser_style(const char* html) {
    BrowserStyle style = {1, 15};
    if (!html) return style;
    style.background = css_color(html, "background-color", style.background);
    style.background = css_color(html, "background", style.background);
    style.foreground = css_color(html, "color", style.foreground);
    return style;
}

static bool is_tag(const char* p, const char* name, bool closing) {
    const char* n = name;
    if (*p++ != '<') return false;
    if (closing) {
        if (*p++ != '/') return false;
    } else if (*p == '/') {
        return false;
    }
    while (*n) {
        if (ascii_lower(*p++) != ascii_lower(*n++)) return false;
    }
    return *p == '>' || *p == '/' || *p == ' ' || *p == '\t' || *p == '\n';
}

static char decode_entity(const char* p, int* length) {
    static const struct Entity { const char* name; char value; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}
    };
    unsigned int i;
    for (i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
        if (starts_with(p, entities[i].name)) {
            const char* q = entities[i].name;
            int count = 0;
            while (*q++) ++count;
            *length = count;
            return entities[i].value;
        }
    }
    *length = 0;
    return 0;
}

void browser_render(const char* html, int x, int y, int width, int max_y,
                    unsigned char color, TextRenderer render_text) {
    if (!html || !render_text) return;
    bool heading = false;
    bool in_style = false;
    bool in_script = false;
    bool in_title = false;
    int cursor_x = x;
    int cursor_y = y;

    for (const char* p = html; *p && cursor_y < max_y;) {
        if (starts_with(p, "<!--")) {
            p += 4;
            while (*p && !starts_with(p, "-->")) ++p;
            if (*p) p += 3;
            continue;
        }
        if (*p == '<') {
            const char* end = p;
            while (*end && *end != '>') ++end;
            bool closing = p[1] == '/';
            if (closing) {
                if (is_tag(p, "h1", true) || is_tag(p, "h2", true) ||
                    is_tag(p, "h3", true)) heading = false;
                if (is_tag(p, "style", true)) in_style = false;
                if (is_tag(p, "script", true)) in_script = false;
                if (is_tag(p, "title", true)) in_title = false;
                if (is_tag(p, "p", true) || is_tag(p, "div", true) ||
                    is_tag(p, "li", true)) {
                    cursor_x = x;
                    cursor_y += 9;
                }
            } else {
                if (is_tag(p, "style", false)) in_style = true;
                else if (is_tag(p, "script", false)) in_script = true;
                else if (is_tag(p, "title", false)) in_title = true;
                else if (is_tag(p, "h1", false) || is_tag(p, "h2", false) ||
                         is_tag(p, "h3", false)) {
                    if (cursor_x != x) { cursor_x = x; cursor_y += 8; }
                    cursor_y += 2;
                    heading = true;
                } else if (is_tag(p, "p", false) || is_tag(p, "div", false) ||
                           is_tag(p, "li", false)) {
                    if (cursor_x != x) { cursor_x = x; cursor_y += 8; }
                    cursor_y += 2;
                    if (is_tag(p, "li", false)) {
                        render_text(cursor_x, cursor_y, "- ", color);
                        cursor_x += 12;
                    }
                } else if (is_tag(p, "br", false) || is_tag(p, "hr", false)) {
                    cursor_x = x;
                    cursor_y += 9;
                } else if (is_tag(p, "button", false)) {
                    if (cursor_x + 6 > x + width) {
                        cursor_x = x;
                        cursor_y += 9;
                    }
                    render_text(cursor_x, cursor_y, "[", color);
                    cursor_x += 6;
                } else if (is_tag(p, "button", true)) {
                    render_text(cursor_x, cursor_y, "]", color);
                    cursor_x += 6;
                }
            }
            p = *end ? end + 1 : end;
            continue;
        }

        if (!in_style && !in_script && !in_title && *p != '\r') {
            char c = *p;
            int entity_length = 0;
            if (*p == '&') {
                char decoded = decode_entity(p, &entity_length);
                if (entity_length) {
                    c = decoded;
                    p += entity_length - 1;
                }
            }
            if (c == '\n') {
                cursor_x = x;
                cursor_y += 9;
            } else {
                if (cursor_x + 6 > x + width) {
                    cursor_x = x;
                    cursor_y += heading ? 10 : 9;
                }
                char glyph[2] = {c, 0};
                render_text(cursor_x, cursor_y, glyph, heading ? 14 : color);
                cursor_x += 6;
            }
        }
        ++p;
    }
}

const char* javascript_result(const char* html) {
    static char output[40];
    static const char ready[] = "JS: SCRIPT READY";
    static const char wrote[] = "JS: DOCUMENT.WRITE";
    if (!html) return ready;

    const char* p = find_text(html, "document.write");
    if (p) {
        unsigned int i = 0;
        while (*p && *p != '(') ++p;
        if (*p == '(') ++p;
        while (*p && *p != ')' && i + 1 < sizeof(output)) {
            if (*p == ' ' || *p == '\t' || *p == '+' || *p == ',') {
                ++p;
                continue;
            }
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                while (*p && *p != quote && i + 1 < sizeof(output)) {
                    if (*p == '\\' && p[1]) ++p;
                    output[i++] = *p++;
                }
                if (*p == quote) ++p;
            } else {
                ++p;
            }
        }
        output[i] = 0;
        return i ? output : wrote;
    }
    if (contains(html, "document.title")) return "JS: TITLE ASSIGNED";
    if (contains(html, "<script")) return ready;
    return "JS: NO SCRIPT";
}

const char* javascript_click_result(const char* html) {
    static char output[40];
    const char* p = find_text(html, "onclick");
    if (!p) return "JS: NO CLICK HANDLER";
    while (*p && *p != '=') ++p;
    if (*p == '=') ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"' && *p != '\'') return "JS: CLICK HANDLER FOUND";
    char attribute_quote = *p++;
    const char* end = p;
    while (*end && *end != attribute_quote) ++end;
    const char* call = p;
    while (call < end && !starts_with(call, "alert(") &&
           !starts_with(call, "document.write(")) ++call;
    if (call == end) return "JS: CLICK HANDLER FOUND";
    while (call < end && *call != '(') ++call;
    if (call < end) ++call;
    while (call < end && *call != '\'' && *call != '"') ++call;
    if (call == end) return "JS: CLICK HANDLER FIRED";
    char quote = *call++;
    unsigned int i = 0;
    static const char prefix[] = "JS: ";
    while (i < sizeof(prefix) - 1) {
        output[i] = prefix[i];
        ++i;
    }
    while (call < end && *call != quote && i + 1 < sizeof(output)) {
        if (*call == '\\' && call + 1 < end) ++call;
        output[i++] = *call++;
    }
    output[i] = 0;
    return output;
}

}
