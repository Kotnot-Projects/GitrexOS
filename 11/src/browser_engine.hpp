#ifndef GITREX_BROWSER_ENGINE_HPP
#define GITREX_BROWSER_ENGINE_HPP

namespace gitrex {

struct BrowserStyle {
    unsigned char background;
    unsigned char foreground;
};

typedef void (*TextRenderer)(int x, int y, const char* text, unsigned char color);

BrowserStyle browser_style(const char* html);
bool browser_contains(const char* text, const char* needle);
void browser_render(const char* html, int x, int y, int width, int max_y,
                    unsigned char color, TextRenderer render_text);
const char* javascript_result(const char* html);
const char* javascript_click_result(const char* html);

}

#endif
