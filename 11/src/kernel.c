typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#include "browser_engine.hpp"
#include "network_stack.hpp"

#define WIDTH 320
#define HEIGHT 200
#define FB ((volatile u8*)0xA0000)
#define KBD_DATA 0x60
#define KBD_STATUS 0x64
#define MOUSE_PACKET_SIZE 3
#define FS_MAX_FILES 8
#define FS_NAME_SIZE 16
#define FS_DATA_SIZE 256

static void key_action(u8 code);
static void mouse_click(u8 buttons);
static void mouse_context(void);
static void paint_draw_at(int x, int y);
static void calculator_input(char c);
static void pixel(int x, int y, u8 color);
static void network_send_ethernet(const u8* frame, u16 length);
static void network_poll(void);

enum App {
    APP_DESKTOP = 0,
    APP_PAINT,
    APP_FILES,
    APP_CALCULATOR,
    APP_TERMINAL,
    APP_SETTINGS,
    APP_BROWSER,
    APP_SNAKE,
    APP_SHOOTER,
    APP_NOTEPAD
};

static enum App active_app = APP_DESKTOP;
static u32 ticks;
static char terminal_line[64];
static u8 terminal_length;
static char calculator_line[32];
static u8 calculator_length;
static char calculator_result[32] = "0";
static char notepad_buffer[256];
static u16 notepad_length;
static char notepad_status[20] = "UNSAVED";
static u8 keyboard_ctrl;
static u8 selected_file;
static u8 fs_initialized;
static u8 terminal_log_count;
static char terminal_log[3][40];
static u8 terminal_log_color[3];
static u8 paint_color = 15;
static u8 paint_canvas[256 * 86];
static u8 snake_x[32], snake_y[32], snake_length = 4, snake_direction = 1;
static u8 snake_food_x = 18, snake_food_y = 8;
static u32 snake_score;
static u32 game_delay;
static u8 shooter_player_x = 156;
static u8 shooter_bullet_x = 0, shooter_bullet_y = 0, shooter_bullet_active;
static u8 shooter_target_x = 46, shooter_target_y = 60;
static u32 shooter_score;
static u8 snake_alive = 1;
static int notepad_file_index = -1;
static int mouse_x = WIDTH / 2;
static int mouse_y = HEIGHT / 2;
static u8 mouse_buttons;
static u8 mouse_packet[MOUSE_PACKET_SIZE];
static u8 mouse_cycle;
static u8 mouse_present;
static u8 keyboard_shift;
static u8 keyboard_caps;
static u8 keyboard_alt;
static u8 keyboard_russian;
static u8 keyboard_layout_chord_active;
static u8 keyboard_extended;
static u8 screen_dirty = 1;
static u16 rtl8139_base;
static u8 network_ready;
static u8 rtl8821ce_seen;
static u32 network_rx_packets;
static const char* network_status = "NO NIC";
static u8 network_mac[6];
static u8 rtl_rx_buffer[8192 + 16] __attribute__((aligned(4)));
static u8 rtl_tx_buffers[4][1536] __attribute__((aligned(4)));
static u8 rtl_tx_index;
static u16 rtl_rx_offset;
static u8 network_rx_frame[1536];

struct FsFile {
    char name[FS_NAME_SIZE];
    char data[FS_DATA_SIZE];
    u16 size;
    u8 used;
};
static FsFile fs_files[FS_MAX_FILES];

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline u32 inl(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void delay(u32 count) {
    while (count--) __asm__ volatile ("pause");
}

static void memset8(volatile u8* dst, u8 value, u32 count) {
    while (count--) *dst++ = value;
}

static u16 string_length(const char* value) {
    u16 length = 0;
    while (value[length] && length < 65535) length++;
    return length;
}

static u8 string_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return (u8)(*a == *b);
}

static void string_copy(char* dst, const char* src, u16 capacity) {
    u16 i = 0;
    if (!capacity) return;
    while (src[i] && i + 1 < capacity) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int fs_find(const char* name) {
    int i;
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used && string_equal(fs_files[i].name, name)) return i;
    }
    return -1;
}

static int fs_save(const char* name, const char* data, u16 length) {
    int index = fs_find(name);
    u16 i;
    if (index < 0) {
        for (index = 0; index < FS_MAX_FILES && fs_files[index].used; index++) {}
        if (index == FS_MAX_FILES) return 0;
    }
    string_copy(fs_files[index].name, name, FS_NAME_SIZE);
    if (length >= FS_DATA_SIZE) length = FS_DATA_SIZE - 1;
    for (i = 0; i < length; i++) fs_files[index].data[i] = data[i];
    fs_files[index].data[length] = 0;
    fs_files[index].size = length;
    fs_files[index].used = 1;
    return 1;
}

static void fs_delete(int index) {
    u16 i;
    if (index < 0 || index >= FS_MAX_FILES) return;
    fs_files[index].used = 0;
    fs_files[index].size = 0;
    fs_files[index].name[0] = 0;
    for (i = 0; i < FS_DATA_SIZE; i++) fs_files[index].data[i] = 0;
}

static void fs_init(void) {
    static const char readme[] = "GITREXOS RAM FILESYSTEM";
    static const char welcome[] =
        "TRM1\necho GitrexOS terminal\napps\nabout";
    if (fs_initialized) return;
    fs_save("README.TXT", readme, sizeof(readme) - 1);
    fs_save("NOTE.TXT", "", 0);
    fs_save("WELCOME.TRM", welcome, sizeof(welcome) - 1);
    fs_initialized = 1;
}

static void terminal_message(const char* message, u8 color) {
    u8 i;
    if (terminal_log_count == 3) {
        for (i = 0; i < 2; i++) {
            string_copy(terminal_log[i], terminal_log[i + 1], sizeof(terminal_log[i]));
            terminal_log_color[i] = terminal_log_color[i + 1];
        }
        terminal_log_count = 2;
    }
    string_copy(terminal_log[terminal_log_count], message, sizeof(terminal_log[0]));
    terminal_log_color[terminal_log_count++] = color;
}

static u8 ps2_wait_input_clear(void) {
    u32 timeout = 100000;
    while ((inb(KBD_STATUS) & 2) && timeout--) delay(10);
    return timeout != 0;
}

static u8 ps2_wait_output_ready(void) {
    u32 timeout = 100000;
    while (!(inb(KBD_STATUS) & 1) && timeout--) delay(10);
    return timeout != 0;
}

static u8 ps2_mouse_command(u8 command) {
    if (!ps2_wait_input_clear()) return 0;
    outb(KBD_STATUS, 0xD4);
    if (!ps2_wait_input_clear()) return 0;
    outb(KBD_DATA, command);
    if (!ps2_wait_output_ready()) return 0;
    return inb(KBD_DATA) == 0xFA;
}

static void mouse_init(void) {
    u8 config;
    if (!ps2_wait_input_clear()) return;
    outb(KBD_STATUS, 0xA8);
    if (!ps2_wait_input_clear()) return;
    outb(KBD_STATUS, 0x20);
    if (!ps2_wait_output_ready()) return;
    config = inb(KBD_DATA);
    config |= 2;
    if (!ps2_wait_input_clear()) return;
    outb(KBD_STATUS, 0x60);
    if (!ps2_wait_input_clear()) return;
    outb(KBD_DATA, config);
    if (ps2_mouse_command(0xF6) && ps2_mouse_command(0xF4)) {
        mouse_present = 1;
    }
}

static void mouse_poll(void) {
    u8 status;
    u8 buttons;
    int dx;
    int dy;
    while ((status = inb(KBD_STATUS)) & 1) {
        if (!(status & 0x20)) break;
        u8 value = inb(KBD_DATA);
        if (mouse_cycle == 0 && !(value & 8)) continue;
        mouse_packet[mouse_cycle++] = value;
        if (mouse_cycle != MOUSE_PACKET_SIZE) continue;
        mouse_cycle = 0;
        buttons = mouse_packet[0] & 7;
        dx = mouse_packet[1];
        dy = mouse_packet[2];
        if (mouse_packet[0] & 0x10) dx -= 256;
        if (mouse_packet[0] & 0x20) dy -= 256;
        mouse_x += dx / 2;
        mouse_y -= dy / 2;
        if (mouse_x < 1) mouse_x = 1;
        if (mouse_y < 1) mouse_y = 1;
        if (mouse_x > WIDTH - 2) mouse_x = WIDTH - 2;
        if (mouse_y > HEIGHT - 2) mouse_y = HEIGHT - 2;
        if (dx || dy || buttons != mouse_buttons) screen_dirty = 1;
        if ((buttons & 1) && !(mouse_buttons & 1)) mouse_click(1);
        if ((buttons & 2) && !(mouse_buttons & 2)) mouse_context();
        if ((buttons & 1) && active_app == APP_PAINT) paint_draw_at(mouse_x, mouse_y);
        mouse_buttons = buttons;
    }
}

static void draw_cursor(void) {
    int i;
    for (i = 0; i < 12; i++) {
        pixel(mouse_x, mouse_y + i, 15);
        if (i < 8) pixel(mouse_x + i, mouse_y + i, 15);
        if (i > 2 && i < 9) pixel(mouse_x + 2, mouse_y + i, 0);
    }
}

static u32 pci_read(u8 bus, u8 slot, u8 function, u8 offset) {
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
                  ((u32)function << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_write(u8 bus, u8 slot, u8 function, u8 offset, u32 value) {
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
                  ((u32)function << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

static void network_init(void) {
    u8 slot;
    u8 i;
    u32 id;
    u32 bar;
    u32 command;
    for (slot = 0; slot < 32; slot++) {
        id = pci_read(0, slot, 0, 0);
        if ((id & 0xFFFF) == 0x10EC &&
            (((id >> 16) & 0xFFFF) == 0xB821 ||
             ((id >> 16) & 0xFFFF) == 0xC821)) {
            rtl8821ce_seen = 1;
        }
        if (network_ready) continue;
        if ((id & 0xFFFF) != 0x10EC || ((id >> 16) & 0xFFFF) != 0x8139) continue;
        bar = pci_read(0, slot, 0, 0x10);
        if (!(bar & 1)) continue;
        rtl8139_base = (u16)(bar & 0xFFFC);
        if (!rtl8139_base) continue;
        command = pci_read(0, slot, 0, 0x04);
        pci_write(0, slot, 0, 0x04, command | 0x00000005);
        outb(rtl8139_base + 0x52, 0);
        outb(rtl8139_base + 0x37, 0x10);
        delay(10000);
        for (i = 0; i < 6; i++) network_mac[i] = inb(rtl8139_base + i);
        outl(rtl8139_base + 0x30, (u32)rtl_rx_buffer);
        rtl_rx_offset = 0;
        outw(rtl8139_base + 0x38, 0xFFF0);
        outl(rtl8139_base + 0x44, 0x0000008F);
        outw(rtl8139_base + 0x3E, 0xFFFF);
        outb(rtl8139_base + 0x37, 0x0C);
        outw(rtl8139_base + 0x3C, 0);
        network_ready = 1;
        network_status = "QEMU NIC 10.0.2.15";
    }
    if (network_ready) {
        gitrex::network_stack_start(network_send_ethernet, network_poll, network_mac);
    } else if (rtl8821ce_seen) {
        network_status = "RTL8821CE - DRIVER MISSING";
    }
}

static void network_send_ethernet(const u8* frame, u16 length) {
    u8* target;
    u16 i;
    if (!network_ready || length > 1536) return;
    target = rtl_tx_buffers[rtl_tx_index];
    for (i = 0; i < length; i++) target[i] = frame[i];
    outl(rtl8139_base + 0x20 + rtl_tx_index * 4, (u32)target);
    outl(rtl8139_base + 0x10 + rtl_tx_index * 4, length);
    rtl_tx_index = (u8)((rtl_tx_index + 1) & 3);
}

static u8 rtl_rx_read(u16 offset) {
    return rtl_rx_buffer[offset & 8191];
}

static void network_poll(void) {
    u16 current;
    u16 isr;
    u8 packets = 0;
    if (!network_ready) return;
    current = (u16)(inw(rtl8139_base + 0x3A) & 8191);
    while (rtl_rx_offset != current && packets++ < 32) {
        u16 packet_status = (u16)(rtl_rx_read(rtl_rx_offset) |
                                  ((u16)rtl_rx_read((u16)(rtl_rx_offset + 1)) << 8));
        u16 packet_length = (u16)(rtl_rx_read((u16)(rtl_rx_offset + 2)) |
                                  ((u16)rtl_rx_read((u16)(rtl_rx_offset + 3)) << 8));
        u16 frame_length;
        u16 i;
        if (!(packet_status & 1) || packet_length < 4 || packet_length > 1536) {
            rtl_rx_offset = current;
            outw(rtl8139_base + 0x38, (u16)(rtl_rx_offset - 16));
            break;
        }
        frame_length = (u16)(packet_length - 4);
        for (i = 0; i < frame_length; i++) {
            network_rx_frame[i] = rtl_rx_read((u16)(rtl_rx_offset + 4 + i));
        }
        rtl_rx_offset = (u16)((rtl_rx_offset + 4 + packet_length + 3) & ~3);
        rtl_rx_offset &= 8191;
        outw(rtl8139_base + 0x38, (u16)(rtl_rx_offset - 16));
        network_rx_packets++;
        gitrex::network_stack_input(network_rx_frame, frame_length);
        current = (u16)(inw(rtl8139_base + 0x3A) & 8191);
    }
    isr = inw(rtl8139_base + 0x3E);
    if (isr) outw(rtl8139_base + 0x3E, isr);
}

static void rect(int x, int y, int w, int h, u8 color) {
    int yy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > WIDTH) w = WIDTH - x;
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++) memset8(FB + (y + yy) * WIDTH + x, color, (u32)w);
}

static void pixel(int x, int y, u8 color) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) FB[y * WIDTH + x] = color;
}

static u8 glyph_row(char c, int row) {
    static const u8 cyrillic[33][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {30,17,17,30,17,17,30}, {31,16,16,16,16,16,16},
        {14,17,17,17,17,17,31}, {31,16,16,30,16,16,31},
        {21,21,14,4,14,21,21}, {30,1,2,12,2,1,30},
        {17,25,21,19,17,17,17}, {4,10,0,17,25,21,19},
        {17,18,20,24,20,18,17}, {7,9,9,9,17,17,17},
        {17,27,21,21,17,17,17}, {17,17,17,31,17,17,17},
        {14,17,17,17,17,17,14}, {31,17,17,17,17,17,17},
        {30,17,17,30,16,16,16}, {15,16,16,16,16,16,15},
        {31,4,4,4,4,4,4}, {17,17,17,15,1,1,30},
        {4,14,21,21,14,4,4}, {17,17,10,4,10,17,17},
        {17,17,17,17,17,17,31}, {17,17,17,15,1,1,1},
        {21,21,21,21,21,21,31}, {21,21,21,21,21,21,31},
        {16,16,16,30,17,17,30}, {17,17,17,29,21,21,29},
        {16,16,16,30,17,17,30}, {30,1,1,15,1,1,30},
        {17,21,21,29,21,21,17}, {15,17,17,15,5,9,17},
        {10,0,31,16,30,16,31}
    };
    u8 byte = (u8)c;
    int index = -1;
    static const u8 digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    static const u8 letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {1,1,1,1,17,17,14}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    if (byte >= 0x80 && byte <= 0x9F) index = byte - 0x80;
    else if (byte >= 0xA0 && byte <= 0xAF) index = byte - 0xA0;
    else if (byte >= 0xE0 && byte <= 0xEF) index = byte - 0xE0 + 16;
    else if (byte == 0xF0 || byte == 0xF1) index = 32;
    if (index >= 0) return cyrillic[index][row];
    if (byte >= '0' && byte <= '9') return digits[byte - '0'][row];
    if (byte >= 'A' && byte <= 'Z') return letters[byte - 'A'][row];
    if (byte >= 'a' && byte <= 'z') return letters[byte - 'a'][row];
    if (c == '-') return row == 3 ? 31 : 0;
    if (c == '+') return row == 3 ? 31 : ((row >= 1 && row <= 5) ? 4 : 0);
    if (c == '*') return row == 1 || row == 5 ? 21 : (row == 2 || row == 4 ? 14 : (row == 3 ? 4 : 0));
    if (c == '=') return row == 2 || row == 4 ? 31 : 0;
    if (c == '|') return 4;
    if (c == '(') return row == 0 || row == 6 ? 2 : (row == 1 || row == 5 ? 4 : 8);
    if (c == ')') return row == 0 || row == 6 ? 8 : (row == 1 || row == 5 ? 4 : 2);
    if (c == '\'') return row < 2 ? 4 : 0;
    if (c == '"') return row < 2 ? 10 : 0;
    if (c == ',') return row == 6 ? 8 : 0;
    if (c == '%') return row == 0 || row == 6 ? 17 :
                           (row == 1 || row == 5 ? 2 :
                           (row == 2 || row == 4 ? 4 : 8));
    if (c == ':') return (row == 2 || row == 5) ? 4 : 0;
    if (c == ';') return (row == 2 || row == 5) ? 4 : (row == 6 ? 8 : 0);
    if (c == '.') return row == 6 ? 4 : 0;
    if (c == '/') {
        static const u8 slash[7] = {1,2,2,4,8,8,16};
        return slash[row];
    }
    if (c == '\\') {
        static const u8 backslash[7] = {16,8,8,4,2,2,1};
        return backslash[row];
    }
    if (c == '>') return row == 3 ? 14 : (row == 2 ? 7 : (row == 4 ? 7 : 0));
    if (c == '<') return row == 3 ? 14 : (row == 2 ? 28 : (row == 4 ? 28 : 0));
    if (c == '!') return row < 5 ? 4 : (row == 6 ? 4 : 0);
    if (c == '?') return row == 0 ? 14 : (row == 1 ? 17 : (row == 2 ? 2 : (row == 3 ? 4 : (row == 6 ? 4 : 0))));
    if (c == '_') return row == 6 ? 31 : 0;
    return byte == ' ' ? 0 : (row == 0 || row == 6 ? 31 : 17);
}

static void text(int x, int y, const char* value, u8 color) {
    int cursor = x;
    int row;
    while (*value) {
        char c = *value++;
        for (row = 0; row < 7; row++) {
            u8 bits = glyph_row(c, row);
            int col;
            for (col = 0; col < 5; col++) if (bits & (1 << (4 - col))) pixel(cursor + col, y + row, color);
        }
        cursor += 6;
    }
}

static void frame(int x, int y, int w, int h, const char* title) {
    rect(x, y, w, h, 8);
    rect(x + 2, y + 2, w - 4, 12, 24);
    rect(x + 2, y + 14, w - 4, h - 16, 1);
    text(x + 7, y + 5, title, 15);
}

static void desktop(void) {
    rect(0, 0, WIDTH, HEIGHT, 17);
    rect(0, 0, WIDTH, 18, 24);
    text(8, 5, "GITREXOS", 15);
    text(242, 5, "READY", 10);
    rect(0, 180, WIDTH, 20, 24);
    text(8, 182, "1 PAINT  2 FILES  3 CALC  4 TERM", 15);
    text(8, 191, "5 SET  6 WEB  7 SNAKE  8 SHOOT  9 NOTE", 15);
    frame(28, 34, 264, 126, "WELCOME TO GITREXOS");
    text(48, 61, "A GRAPHICAL ASSEMBLY + C OS", 14);
    text(48, 78, "PRESS 1-9 TO OPEN AN APP", 11);
    text(48, 94, "GP PROGRAMS USE .GP FORMAT", 10);
    text(48, 110, "TERMINAL SCRIPTS USE .TRM", 10);
    text(48, 133, "BUILD 01  |  VGA 320X200", 7);
}

static void paint_app(void) {
    int x, y;
    frame(12, 24, 296, 150, "PAINT.GP");
    rect(24, 46, 272, 108, 0);
    rect(28, 49, 14, 14, 4); rect(48, 49, 14, 14, 12); rect(68, 49, 14, 14, 14);
    rect(88, 49, 14, 14, 9); rect(108, 49, 14, 14, 2); rect(128, 49, 14, 14, 15);
    rect(28 + (paint_color == 4 ? 0 : paint_color == 12 ? 20 : paint_color == 14 ? 40 :
             paint_color == 9 ? 60 : paint_color == 2 ? 80 : 100), 48, 14, 2, 15);
    text(151, 53, "C CLEAR", 15);
    for (y = 0; y < 76; y++) {
        for (x = 0; x < 256; x++) {
            u8 color = paint_canvas[y * 256 + x];
            if (color) pixel(28 + x, 72 + y, color);
        }
    }
    text(10, 184, "ESC DESKTOP  |  C CLEAR CANVAS", 15);
}

static void files_app(void) {
    int row = 0;
    int i;
    frame(12, 24, 296, 150, "FILES.GP");
    text(22, 45, "UP DOWN  N NEW  ENTER EDIT  DEL DELETE", 11);
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used) continue;
        int y = 60 + row * 13;
        if (row == selected_file) rect(20, y - 2, 276, 11, 24);
        text(24, y, fs_files[i].name, row == selected_file ? 15 : 10);
        text(190, y, "TXT", 7);
        row++;
    }
    text(22, 150, row ? "ENTER EDIT  DEL REMOVE  RAM ONLY" : "EMPTY - PRESS N TO CREATE", 7);
}

static void calculator_app(void) {
    frame(50, 26, 220, 144, "CALCULATOR.GP");
    rect(68, 48, 184, 23, 0);
    text(78, 56, calculator_length ? calculator_line : calculator_result, 14);
    rect(72, 82, 192, 60, 8);
    text(82, 88, "7  8  9  /", 15);
    text(82, 101, "4  5  6  *", 15);
    text(82, 114, "1  2  3  -", 15);
    text(82, 127, "0  C  =  +", 15);
    text(10, 184, "CLICK KEYS OR TYPE  ENTER = RESULT", 15);
}

static void terminal_app(void) {
    frame(10, 24, 300, 150, "TERMINAL.GP");
    rect(18, 42, 284, 118, 0);
    text(24, 48, "GITREXOS TERMINAL  .TRM ENABLED", 10);
    for (u8 i = 0; i < terminal_log_count; i++) {
        text(24, 61 + i * 10, terminal_log[i], terminal_log_color[i]);
    }
    text(24, 100, "GITREX> ", 15);
    text(72, 100, terminal_line, 14);
    text(10, 184, "HELP LS APPS ABOUT ECHO CLEAR REBOOT SHUTDOWN", 15);
}

static void settings_app(void) {
    frame(22, 25, 276, 148, "SETTINGS.GP");
    text(38, 45, "DISPLAY", 14); text(190, 45, "VGA 320X200", 10);
    text(38, 62, "THEME", 14); text(190, 62, "MIDNIGHT", 10);
    text(38, 79, "LAYOUT", 14); text(190, 79, keyboard_russian ? "RUSSIAN" : "ENGLISH", 10);
    text(38, 96, "CAPS LOCK", 14); text(190, 96, keyboard_caps ? "ON" : "OFF", 10);
    text(38, 113, "MEMORY", 14); text(190, 113, "32 MB", 10);
    text(38, 130, "MOUSE", 14); text(190, 130, mouse_present ? "PS2 READY" : "PS2 OFF", 10);
    text(38, 147, "NETWORK", 14); text(150, 147, network_status, 10);
    text(38, 160, rtl8821ce_seen ? "RTL8821CE FOUND - WIFI DRIVER NEEDED" :
                                  "ALT+SHIFT LAYOUT  |  NO WIFI DRIVER", 7);
}

static char browser_address[64] = "gp://home";
static u8 browser_address_length = 9;
static u8 browser_address_focus;
static const char* browser_html;
static const char* browser_dynamic_text = "JS: DOM READY";
static u8 browser_page_bg = 1;
static u8 browser_page_text = 15;
static char browser_remote_page[4096];

static void browser_load(void) {
    u8 local_page = 0;
    u8 remote_page = 0;
    if (gitrex::browser_contains(browser_address, "about:network")) {
        browser_html = "<h1>GITREXOS NETWORK</h1><p>RTL8139 DRIVER STATUS</p><p>Use QEMU with an RTL8139 device.</p>";
        browser_page_bg = 1;
        browser_page_text = 15;
        browser_dynamic_text = network_ready ? "QEMU STATIC IP DNS TCP HTTP" :
                               rtl8821ce_seen ? "RTL8821CE FOUND - WIFI DRIVER NEEDED" :
                                                "NO NETWORK ADAPTER";
    } else if (gitrex::browser_contains(browser_address, "https://")) {
        browser_html = "<h1>HTTPS UNAVAILABLE</h1>"
                       "<p>THIS BUILD SUPPORTS PLAIN HTTP ONLY.</p>"
                       "<p>TLS IS REQUIRED FOR SECURE SITES.</p>";
        browser_page_bg = 4;
        browser_page_text = 15;
        browser_dynamic_text = "TLS NOT IMPLEMENTED";
    } else if (gitrex::browser_contains(browser_address, "http://")) {
        if (network_ready && gitrex::network_http_get(browser_address,
                                                       browser_remote_page,
                                                       sizeof(browser_remote_page))) {
            browser_html = browser_remote_page;
            local_page = 1;
            remote_page = 1;
            network_status = "QEMU HTTP READY";
        } else {
            browser_html = "<h1>HTTP LOAD FAILED</h1>"
                           "<p>CHECK QEMU NETWORK AND ADDRESS.</p>"
                           "<p>HTTPS IS NOT SUPPORTED.</p>";
            browser_page_bg = 4;
            browser_page_text = 15;
            browser_dynamic_text = "DNS OR TCP REQUEST FAILED";
            network_status = network_ready ? "QEMU HTTP FAILED" : "NO NETWORK ADAPTER";
        }
    } else if (gitrex::browser_contains(browser_address, "www.")) {
        browser_html = "<h1>EXTERNAL SITE UNAVAILABLE</h1>"
                       "<p>ENTER A FULL HTTP:// ADDRESS.</p>"
                       "<p>HTTPS SITES NEED TLS SUPPORT.</p>";
        browser_page_bg = 4;
        browser_page_text = 15;
        browser_dynamic_text = "USE HTTP://HOST/PATH";
    } else if (gitrex::browser_contains(browser_address, "gp://file/")) {
        char filename[FS_NAME_SIZE];
        u16 source = 10;
        u16 target = 0;
        int index;
        while (browser_address[source] && browser_address[source] != '?' &&
               target + 1 < sizeof(filename)) {
            filename[target++] = browser_address[source++];
        }
        filename[target] = 0;
        index = fs_find(filename);
        if (index >= 0) {
            browser_html = fs_files[index].data;
            local_page = 1;
        } else {
            browser_html = "<h1>FILE NOT FOUND</h1><p>USE GP://FILE/NAME.TXT</p>";
            browser_page_bg = 1;
            browser_page_text = 15;
            browser_dynamic_text = "RAM FILE DOES NOT EXIST";
        }
    } else if (!string_equal(browser_address, "gp://home")) {
        browser_html = "<h1>UNKNOWN ADDRESS</h1><p>USE GP://HOME</p>";
        browser_page_bg = 1;
        browser_page_text = 15;
        browser_dynamic_text = "ADDRESS NOT FOUND";
    } else {
        browser_html = "<style>body{background:blue;color:white}</style>"
                       "<h1>GITREXOS BROWSER</h1>"
                       "<p>LOCAL HTML CSS JS ENGINE</p>"
                       "<p>LOAD RAM HTML: GP://FILE/NAME.TXT</p>"
                       "<button onclick=\"alert('BUTTON CLICKED')\">RUN JAVASCRIPT</button>"
                       "<script>document.title='GITREXOS';document.write('JS READY');</script>";
        local_page = 1;
    }
    if (local_page) {
        gitrex::BrowserStyle style = gitrex::browser_style(browser_html);
        browser_page_bg = style.background;
        browser_page_text = style.foreground;
        browser_dynamic_text = remote_page ? "HTTP PAGE LOADED - JS LIMITED" :
                                              gitrex::javascript_result(browser_html);
    }
    browser_address_focus = 0;
}

static void browser_render_html(void) {
    gitrex::browser_render(browser_html, 30, 70, 260, 143,
                           browser_page_text, text);
    text(30, 144, browser_dynamic_text, 10);
}

static u8 paint_palette_color(int x, int y) {
    if (y < 48 || y > 64) return 0;
    if (x >= 28 && x < 42) return 4;
    if (x >= 48 && x < 62) return 12;
    if (x >= 68 && x < 82) return 14;
    if (x >= 88 && x < 102) return 9;
    if (x >= 108 && x < 122) return 2;
    if (x >= 128 && x < 142) return 15;
    return 0;
}

static void paint_draw_at(int x, int y) {
    int canvas_x = x - 28;
    int canvas_y = y - 72;
    if (canvas_x < 0 || canvas_x >= 256 || canvas_y < 0 || canvas_y >= 76) return;
    paint_canvas[canvas_y * 256 + canvas_x] = paint_color;
    screen_dirty = 1;
}

static int fs_index_for_row(int row) {
    int i;
    int visible = 0;
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_files[i].used) continue;
        if (visible++ == row) return i;
    }
    return -1;
}

static int fs_row_count(void) {
    int i;
    int count = 0;
    for (i = 0; i < FS_MAX_FILES; i++) if (fs_files[i].used) count++;
    return count;
}

static void notepad_open_file(int index) {
    u16 i;
    if (index < 0 || index >= FS_MAX_FILES || !fs_files[index].used) return;
    notepad_file_index = index;
    notepad_length = fs_files[index].size;
    if (notepad_length >= sizeof(notepad_buffer)) notepad_length = sizeof(notepad_buffer) - 1;
    for (i = 0; i < notepad_length; i++) notepad_buffer[i] = fs_files[index].data[i];
    notepad_buffer[notepad_length] = 0;
    string_copy(notepad_status, "EDITING", sizeof(notepad_status));
    active_app = APP_NOTEPAD;
}

static void notepad_save(void) {
    if (notepad_file_index >= 0 && notepad_file_index < FS_MAX_FILES &&
        fs_files[notepad_file_index].used &&
        fs_save(fs_files[notepad_file_index].name, notepad_buffer, notepad_length)) {
        string_copy(notepad_status, "SAVED TO RAM", sizeof(notepad_status));
    } else {
        string_copy(notepad_status, "UNSAVED", sizeof(notepad_status));
    }
}

static void files_new(void) {
    static const char* names[] = {
        "NEW.TXT", "NEW1.TXT", "NEW2.TXT", "NEW3.TXT",
        "NEW4.TXT", "NEW5.TXT", "NEW6.TXT", "NEW7.TXT"
    };
    int i;
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (fs_find(names[i]) < 0 && fs_save(names[i], "", 0)) {
            notepad_open_file(fs_find(names[i]));
            return;
        }
    }
    terminal_message("RAM FILE LIST FULL", 12);
}

static void mouse_context(void) {
    screen_dirty = 1;
    if (active_app == APP_PAINT) {
        int palette = paint_palette_color(mouse_x, mouse_y);
        if (palette) paint_color = palette;
        else {
            paint_color = paint_color == 15 ? 4 : (u8)(paint_color + 1);
            if (paint_color == 8 || paint_color == 10 || paint_color == 11 ||
                paint_color == 13) paint_color = 12;
        }
    } else if (active_app == APP_FILES) {
        int index = fs_index_for_row(selected_file);
        if (index >= 0) {
            fs_delete(index);
            selected_file = 0;
        }
    } else if (active_app == APP_BROWSER) {
        browser_address_focus = 0;
    }
}

static void browser_app(void) {
    frame(10, 24, 300, 150, "BROWSER.GP");
    rect(22, 44, 276, 17, 0);
    text(28, 49, browser_address, 14);
    if (browser_address_focus) rect(29 + browser_address_length * 6, 47, 2, 11, 14);
    rect(22, 64, 276, 94, browser_page_bg);
    browser_render_html();
    text(10, 184, "CTRL+L URL  |  ENTER LOAD  |  ESC DESKTOP", 15);
}

static void mouse_click(u8 buttons) {
    if (!(buttons & 1)) return;
    screen_dirty = 1;
    if (active_app == APP_DESKTOP && mouse_y >= 180) {
        int selected = mouse_y < 190 ? mouse_x / 80 + 1 : mouse_x / 64 + 5;
        if (selected >= 1 && selected <= 9) active_app = (enum App)selected;
        return;
    }
    if (active_app == APP_PAINT) {
        u8 color = paint_palette_color(mouse_x, mouse_y);
        if (color) paint_color = color;
        else paint_draw_at(mouse_x, mouse_y);
    } else if (active_app == APP_FILES) {
        if (mouse_y >= 58 && mouse_y < 154) {
            int row = (mouse_y - 58) / 13;
            int index = fs_index_for_row(row);
            if (index >= 0) {
                selected_file = (u8)row;
                if (mouse_x > 160) notepad_open_file(index);
            }
        }
    } else if (active_app == APP_CALCULATOR) {
        static const char keys[4][4] = {
            {'7', '8', '9', '/'}, {'4', '5', '6', '*'},
            {'1', '2', '3', '-'}, {'0', 'C', '=', '+'}
        };
        if (mouse_x >= 78 && mouse_x < 254 && mouse_y >= 82 && mouse_y < 142) {
            int row = (mouse_y - 82) / 15;
            int col = (mouse_x - 78) / 42;
            char key = keys[row < 4 ? row : 3][col < 4 ? col : 3];
            calculator_input(key);
        }
    } else if (active_app == APP_SETTINGS) {
        if (mouse_y >= 76 && mouse_y <= 92) keyboard_russian ^= 1;
        else if (mouse_y >= 93 && mouse_y <= 109) keyboard_caps ^= 1;
    } else if (active_app == APP_BROWSER) {
        if (mouse_y >= 44 && mouse_y < 62) {
            browser_address_focus = 1;
            browser_address_length = 0;
            browser_address[0] = 0;
        } else if (mouse_y >= 64 && mouse_y < 158) {
            browser_dynamic_text = gitrex::javascript_click_result(browser_html);
            browser_address_focus = 0;
        } else {
            browser_address_focus = 0;
        }
    }
}

static void snake_app(void) {
    int i;
    char score[12];
    frame(22, 24, 276, 148, "SNAKE.GP");
    rect(34, 44, 252, 106, 0);
    text(40, 48, "SCORE", 15);
    u32 value = snake_score;
    int n = 0;
    char reverse[12];
    do { reverse[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 11);
    for (i = 0; i < n; i++) score[i] = reverse[n - i - 1];
    score[n] = 0;
    text(76, 48, score, 15);
    for (i = 0; i < snake_length; i++) {
        rect(38 + snake_x[i] * 10, 58 + snake_y[i] * 9, 8, 7, i ? 10 : 12);
    }
    rect(38 + snake_food_x * 10, 58 + snake_food_y * 9, 7, 7, 14);
    if (!snake_alive) text(112, 100, "GAME OVER", 12);
    text(40, 158, "N NEW GAME  ARROWS MOVE", 15);
}

static void shooter_app(void) {
    char score[12];
    u32 value = shooter_score;
    int i, n = 0;
    char reverse[12];
    frame(12, 24, 296, 150, "SHOOTER.GP");
    rect(24, 44, 272, 114, 0);
    do { reverse[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 11);
    for (i = 0; i < n; i++) score[i] = reverse[n - i - 1];
    score[n] = 0;
    text(34, 50, "SCORE", 15);
    text(70, 50, score, 15);
    rect(34 + shooter_target_x, shooter_target_y, 14, 10, 12);
    rect(37 + shooter_target_x, shooter_target_y + 10, 8, 4, 14);
    if (shooter_bullet_active) rect(34 + shooter_bullet_x, shooter_bullet_y, 3, 6, 15);
    rect(shooter_player_x, 132, 10, 14, 10);
    text(34, 164, "F FIRE  ARROWS MOVE", 15);
}

static void notepad_app(void) {
    int x = 24;
    int y = 48;
    u16 i;
    frame(10, 24, 300, 150, "NOTEPAD.GP");
    rect(18, 42, 284, 118, 0);
    for (i = 0; i < notepad_length && y < 153; i++) {
        char one[2] = {notepad_buffer[i], 0};
        if (notepad_buffer[i] == '\n' || x > 286) {
            x = 24;
            y += 10;
            if (notepad_buffer[i] == '\n') continue;
        }
        text(x, y, one, 15);
        x += 6;
    }
    text(20, 164, notepad_status, 15);
}

static void app_shell(void) {
    rect(0, 0, WIDTH, HEIGHT, 17);
    rect(0, 0, WIDTH, 18, 24);
    text(8, 5, "GITREXOS", 15);
    text(242, 5, "RUNNING", 10);
}

static void render(void) {
    if (active_app != APP_DESKTOP) app_shell();
    switch (active_app) {
        case APP_PAINT: paint_app(); break;
        case APP_FILES: files_app(); break;
        case APP_CALCULATOR: calculator_app(); break;
        case APP_TERMINAL: terminal_app(); break;
        case APP_SETTINGS: settings_app(); break;
        case APP_BROWSER: browser_app(); break;
        case APP_SNAKE: snake_app(); break;
        case APP_SHOOTER: shooter_app(); break;
        case APP_NOTEPAD: notepad_app(); break;
        default: desktop(); break;
    }
}

static void append_terminal(char c) {
    if (terminal_length < sizeof(terminal_line) - 1) {
        terminal_line[terminal_length++] = c;
        terminal_line[terminal_length] = 0;
    }
}

static void append_notepad(char c) {
    if (notepad_length < sizeof(notepad_buffer) - 1) {
        notepad_buffer[notepad_length++] = c;
        notepad_buffer[notepad_length] = 0;
    }
}

static u8 text_equal_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return (u8)(*a == *b);
}

static u8 text_starts_ci(const char* value, const char* prefix) {
    while (*prefix) {
        char a = *value++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void terminal_execute_line(const char* command, u8 allow_run);

static void terminal_execute_script(int index) {
    char line[64];
    u16 i = 0;
    u16 j = 0;
    const char* data;
    if (index < 0 || index >= FS_MAX_FILES || !fs_files[index].used) {
        terminal_message("SCRIPT NOT FOUND", 12);
        return;
    }
    data = fs_files[index].data;
    if (fs_files[index].size < 4 ||
        data[0] != 'T' || data[1] != 'R' || data[2] != 'M' || data[3] != '1') {
        terminal_message("INVALID TRM HEADER", 12);
        return;
    }
    while (i <= fs_files[index].size) {
        char c = data[i++];
        if (c == '\r') continue;
        if (c == '\n' || c == 0 || i > fs_files[index].size) {
            line[j] = 0;
            if (j && !text_equal_ci(line, "TRM1")) terminal_execute_line(line, 0);
            j = 0;
            if (!c) break;
        } else if (j < sizeof(line) - 1) {
            line[j++] = c;
        }
    }
}

static void terminal_execute_line(const char* command, u8 allow_run) {
    char value[40];
    int index;
    u16 i;
    u16 length = string_length(command);
    while (*command == ' ') command++;
    if (!*command) return;
    if (text_equal_ci(command, "help")) {
        terminal_message("HELP LS CAT TOUCH WRITE RM RUN", 14);
        terminal_message("APPS ABOUT ECHO CLEAR REBOOT", 14);
    } else if (text_equal_ci(command, "ls")) {
        if (!fs_row_count()) terminal_message("NO FILES", 7);
        for (i = 0; i < FS_MAX_FILES; i++) {
            if (fs_files[i].used) terminal_message(fs_files[i].name, 10);
        }
    } else if (text_equal_ci(command, "apps")) {
        terminal_message("PAINT FILES CALC TERMINAL", 10);
        terminal_message("SETTINGS BROWSER SNAKE SHOOTER NOTE", 10);
    } else if (text_equal_ci(command, "about")) {
        terminal_message("GITREXOS 01 - RAM FILESYSTEM", 10);
        terminal_message("FILES ARE LOST AT POWER OFF", 7);
    } else if (text_starts_ci(command, "echo ")) {
        terminal_message(command + 5, 14);
    } else if (text_starts_ci(command, "cat ")) {
        index = fs_find(command + 4);
        if (index < 0) terminal_message("FILE NOT FOUND", 12);
        else terminal_message(fs_files[index].data, 10);
    } else if (text_starts_ci(command, "touch ")) {
        if (!*(command + 6)) terminal_message("USAGE: TOUCH NAME", 12);
        else if (fs_find(command + 6) >= 0) terminal_message("FILE ALREADY EXISTS", 14);
        else if (fs_save(command + 6, "", 0)) terminal_message("FILE CREATED IN RAM", 10);
        else terminal_message("RAM FILE LIST FULL", 12);
    } else if (text_starts_ci(command, "rm ")) {
        index = fs_find(command + 3);
        if (index < 0) terminal_message("FILE NOT FOUND", 12);
        else { fs_delete(index); terminal_message("FILE DELETED", 10); }
    } else if (text_starts_ci(command, "write ")) {
        const char* name = command + 6;
        const char* separator = name;
        while (*separator && *separator != ' ') separator++;
        if (!*separator) {
            terminal_message("USAGE: WRITE NAME TEXT", 12);
        } else {
            u16 name_length = (u16)(separator - name);
            if (!name_length || name_length >= FS_NAME_SIZE) {
                terminal_message("INVALID FILE NAME", 12);
            } else {
                for (i = 0; i < name_length; i++) value[i] = name[i];
                value[name_length] = 0;
                if (fs_save(value, separator + 1, string_length(separator + 1))) {
                    terminal_message("FILE WRITTEN TO RAM", 10);
                } else terminal_message("RAM FILE LIST FULL", 12);
            }
        }
    } else if (text_starts_ci(command, "run ") && allow_run) {
        index = fs_find(command + 4);
        terminal_execute_script(index);
    } else if (text_equal_ci(command, "clear")) {
        terminal_log_count = 0;
    } else if (text_equal_ci(command, "reboot")) {
        terminal_message("REBOOTING", 12);
        outb(0x64, 0xFE);
        for (;;) __asm__ volatile ("hlt");
    } else if (text_equal_ci(command, "shutdown")) {
        terminal_message("POWER OFF", 12);
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        for (;;) __asm__ volatile ("hlt");
    } else {
        if (length > 0) terminal_message("UNKNOWN COMMAND - TYPE HELP", 12);
    }
}

static void calculator_result_set(int value) {
    char reverse[16];
    u8 length = 0;
    u8 i;
    u32 magnitude;
    if (value < 0) {
        calculator_result[0] = '-';
        magnitude = (u32)(-(value + 1)) + 1;
    } else {
        magnitude = (u32)value;
        calculator_result[0] = 0;
    }
    do {
        reverse[length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude && length < sizeof(reverse));
    i = value < 0 ? 1 : 0;
    while (length) calculator_result[i++] = reverse[--length];
    calculator_result[i] = 0;
}

static void calculator_evaluate(void) {
    int total = 0;
    int term = 0;
    int number = 0;
    int sign = 1;
    char operation = '+';
    u8 have_number = 0;
    u8 have_digit = 0;
    u8 divide_error = 0;
    u16 i = 0;
    while (i < calculator_length && calculator_line[i] != '=') {
        char c = calculator_line[i++];
        if (c >= '0' && c <= '9') {
            have_number = 1;
            have_digit = 1;
            if (number < 100000000) number = number * 10 + c - '0';
            continue;
        }
        if (c == ' ') continue;
        if (!have_number) {
            string_copy(calculator_result, "ERROR", sizeof(calculator_result));
            goto done;
        }
        if (operation == '*') {
            if (term && number > 100000000 / (term < 0 ? -term : term)) {
                string_copy(calculator_result, "ERROR", sizeof(calculator_result));
                goto done;
            }
            term *= number;
        } else if (operation == '/') {
            if (!number) { divide_error = 1; break; }
            term /= number;
        } else {
            total += sign * term;
            term = number;
            sign = operation == '-' ? -1 : 1;
        }
        operation = c;
        if (operation != '+' && operation != '-' && operation != '*' && operation != '/') {
            string_copy(calculator_result, "ERROR", sizeof(calculator_result));
            goto done;
        }
        number = 0;
        have_number = 0;
    }
    if (!have_number || !have_digit || divide_error) {
        string_copy(calculator_result, divide_error ? "DIV BY ZERO" : "ERROR",
                    sizeof(calculator_result));
    } else {
        if (operation == '*') term *= number;
        else if (operation == '/') {
            if (!number) divide_error = 1;
            else term /= number;
        } else {
            total += sign * term;
            term = number;
            sign = operation == '-' ? -1 : 1;
        }
        if (divide_error) string_copy(calculator_result, "DIV BY ZERO", sizeof(calculator_result));
        else calculator_result_set(total + sign * term);
    }
done:
    calculator_length = 0;
    calculator_line[0] = 0;
}

static void calculator_input(char c) {
    if (c == 'C') {
        calculator_length = 0;
        calculator_line[0] = 0;
        string_copy(calculator_result, "0", sizeof(calculator_result));
    } else if (c == '=') {
        calculator_evaluate();
    } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*') {
        if (calculator_length < sizeof(calculator_line) - 1) {
            calculator_line[calculator_length++] = c;
            calculator_line[calculator_length] = 0;
        }
    } else if (c == '/') {
        if (calculator_length < sizeof(calculator_line) - 1) {
            calculator_line[calculator_length++] = c;
            calculator_line[calculator_length] = 0;
        }
    }
}

static char keyboard_ascii(u8 code) {
    static const u8 russian_scan[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
        0x26, 0x27, 0x28, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
        0x33, 0x34
    };
    static const u8 russian_cp866[] = {
        0xA9, 0xE6, 0xE3, 0xAA, 0xA5, 0xAD, 0xA3, 0xE8, 0xE9, 0xA7,
        0xE5, 0xEA, 0xA4, 0xEB, 0xA2, 0xA0, 0xAF, 0xE0, 0xAE, 0xAB,
        0xA4, 0xA6, 0xED, 0xEF, 0xE7, 0xE1, 0xAC, 0xA8, 0xE2, 0xEC,
        0xA1, 0xEE
    };
    static const char normal[] = "1234567890-=qwertyuiop[]asdfghjkl;'\\zxcvbnm,./";
    static const u8 scan[] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
        0x24, 0x25, 0x26, 0x27, 0x28, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35
    };
    u8 i;
    if (keyboard_russian) {
        if (code == 0x29) {
            u8 yo = (keyboard_shift ^ keyboard_caps) ? 0xF0 : 0xF1;
            return (char)yo;
        }
        for (i = 0; i < sizeof(russian_scan); i++) {
            if (russian_scan[i] == code) {
                u8 cyrillic = russian_cp866[i];
                if (keyboard_shift ^ keyboard_caps) {
                    if (cyrillic >= 0xA0 && cyrillic <= 0xAF) cyrillic -= 0x20;
                    else if (cyrillic >= 0xE0 && cyrillic <= 0xEF) cyrillic -= 0x50;
                }
                return (char)cyrillic;
            }
        }
        if (keyboard_shift && code >= 0x02 && code <= 0x0B) {
            static const char russian_shift_digits[] = "!\"#;%:?*()";
            return russian_shift_digits[code - 0x02];
        }
    }
    for (i = 0; i < sizeof(scan); i++) {
        if (scan[i] == code) {
            char c = normal[i];
            if (c >= 'a' && c <= 'z' && (keyboard_shift ^ keyboard_caps)) {
                c = (char)(c - 'a' + 'A');
            }
            if (keyboard_shift) {
                if (code == 0x0C) return '_';
                if (code == 0x0D) return '+';
                if (code == 0x1A) return '{';
                if (code == 0x1B) return '}';
                if (code == 0x27) return ':';
                if (code == 0x28) return '"';
                if (code == 0x2B) return '|';
                if (code == 0x29) return '~';
                if (code == 0x33) return '<';
                if (code == 0x34) return '>';
                if (code == 0x35) return '?';
            }
            return c;
        }
    }
    if (code == 0x39) return ' ';
    return 0;
}

static void key_ascii_action(char c) {
    if (!c) return;
    if (active_app == APP_TERMINAL) {
        append_terminal(c);
        return;
    }
    if (active_app == APP_CALCULATOR) {
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' ||
            c == '/' || c == '=') calculator_input(c);
        return;
    }
    if (active_app == APP_NOTEPAD) {
        append_notepad(c);
        string_copy(notepad_status, notepad_file_index >= 0 ? "MODIFIED" : "UNSAVED",
                    sizeof(notepad_status));
        return;
    }
    if (active_app != APP_BROWSER || !browser_address_focus) return;
    if (browser_address_length < 44) {
        browser_address[browser_address_length++] = c;
        browser_address[browser_address_length] = 0;
    }
}

static void key_action(u8 code) {
    if (code & 0x80) return;
    screen_dirty = 1;
    if (code == 0x01) {
        if (active_app == APP_NOTEPAD) notepad_save();
        active_app = APP_DESKTOP;
        browser_address_focus = 0;
        return;
    }
    if (active_app == APP_BROWSER && keyboard_ctrl && code == 0x26) {
        browser_address_focus = 1;
        browser_address_length = 0;
        browser_address[0] = 0;
        return;
    }
    if (active_app == APP_TERMINAL) {
        if (code == 0x1C) {
            char command[64];
            string_copy(command, terminal_line, sizeof(command));
            if (*command) {
                terminal_message(command, 14);
                terminal_execute_line(command, 1);
            }
            terminal_length = 0;
            terminal_line[0] = 0;
            return;
        }
        if (code == 0x0E && terminal_length) terminal_line[--terminal_length] = 0;
        return;
    }
    if (active_app == APP_NOTEPAD) {
        if (keyboard_ctrl && code == 0x1F) { notepad_save(); return; }
        if (code == 0x1C) {
            append_notepad('\n');
            string_copy(notepad_status, "MODIFIED", sizeof(notepad_status));
        }
        if (code == 0x0E && notepad_length) {
            notepad_buffer[--notepad_length] = 0;
            string_copy(notepad_status, "MODIFIED", sizeof(notepad_status));
        }
        return;
    }
    if (active_app == APP_BROWSER && browser_address_focus) {
        if (code == 0x01) { browser_address_focus = 0; return; }
        if (code == 0x1C) { browser_load(); return; }
        if (code == 0x0E) {
            if (browser_address_length) browser_address[--browser_address_length] = 0;
            return;
        }
        return;
    }
    if (active_app != APP_DESKTOP) {
        if (active_app == APP_CALCULATOR && code == 0x0E && calculator_length) {
            calculator_line[--calculator_length] = 0;
        }
        if (active_app == APP_CALCULATOR && code == 0x1C) {
            calculator_input('=');
        }
        if (active_app == APP_CALCULATOR && code == 0x2E) calculator_input('C');
        if (active_app == APP_PAINT && code == 0x2E) {
            u16 i;
            for (i = 0; i < sizeof(paint_canvas); i++) paint_canvas[i] = 0;
        }
        if (active_app == APP_PAINT && code >= 0x02 && code <= 0x07) {
            static const u8 colors[6] = {4, 12, 14, 9, 2, 15};
            paint_color = colors[code - 0x02];
        }
        if (active_app == APP_FILES) {
            int count = fs_row_count();
            if (code == 0x31) files_new();
            else if (code == 0x12 || code == 0x1C) {
                notepad_open_file(fs_index_for_row(selected_file));
            } else if (code == 0x53) {
                int index = fs_index_for_row(selected_file);
                if (index >= 0) fs_delete(index);
                if (selected_file >= fs_row_count() && selected_file) selected_file--;
            } else if (code == 0x0E && count) {
                int index = fs_index_for_row(selected_file);
                if (index >= 0) notepad_open_file(index);
            }
        }
        if (active_app == APP_SNAKE && code == 0x31) {
            snake_length = 4;
            snake_score = 0;
            snake_alive = 1;
            snake_direction = 1;
            snake_food_x = 18;
            snake_food_y = 7;
            for (u8 i = 0; i < snake_length; i++) {
                snake_x[i] = (u8)(10 - i);
                snake_y[i] = 5;
            }
        }
        if (active_app == APP_SHOOTER && code == 0x21 && !shooter_bullet_active) {
            shooter_bullet_x = (u8)(shooter_player_x - 34 + 4);
            shooter_bullet_y = 126;
            shooter_bullet_active = 1;
        }
        return;
    }
    if (code >= 0x02 && code <= 0x0A) active_app = (enum App)(code - 1);
}

static void extended_key_action(u8 code) {
    screen_dirty = 1;
    if (active_app == APP_FILES) {
        int count = fs_row_count();
        if (code == 0x48 && selected_file) selected_file--;
        if (code == 0x50 && selected_file + 1 < count) selected_file++;
        if (code == 0x53) {
            int index = fs_index_for_row(selected_file);
            if (index >= 0) fs_delete(index);
            if (selected_file >= fs_row_count() && selected_file) selected_file--;
        }
    } else if (active_app == APP_SNAKE && snake_alive) {
        if (code == 0x48 && snake_direction != 2) snake_direction = 0;
        if (code == 0x4D && snake_direction != 3) snake_direction = 1;
        if (code == 0x50 && snake_direction != 0) snake_direction = 2;
        if (code == 0x4B && snake_direction != 1) snake_direction = 3;
    } else if (active_app == APP_SHOOTER) {
        if (code == 0x4B && shooter_player_x > 36) shooter_player_x -= 4;
        if (code == 0x4D && shooter_player_x < 250) shooter_player_x += 4;
    }
}

static void keyboard_poll(void) {
    u8 raw;
    char ascii;
    enum App app_before;
    u8 status;
    for (;;) {
        status = inb(KBD_STATUS);
        if (!(status & 1) || (status & 0x20)) return;
        raw = inb(KBD_DATA);
        if (raw == 0xE0) { keyboard_extended = 1; continue; }
        if (raw == 0x2A || raw == 0x36) {
            keyboard_shift = 1;
            if (keyboard_alt && !keyboard_layout_chord_active) {
                keyboard_russian ^= 1;
                keyboard_layout_chord_active = 1;
                screen_dirty = 1;
            }
            continue;
        }
        if (raw == 0xAA || raw == 0xB6) {
            keyboard_shift = 0;
            if (!keyboard_alt) keyboard_layout_chord_active = 0;
            continue;
        }
        if (raw == 0x38 && !keyboard_extended) {
            keyboard_alt = 1;
            if (keyboard_shift && !keyboard_layout_chord_active) {
                keyboard_russian ^= 1;
                keyboard_layout_chord_active = 1;
                screen_dirty = 1;
            }
            continue;
        }
        if (raw == 0xB8 && !keyboard_extended) {
            keyboard_alt = 0;
            if (!keyboard_shift) keyboard_layout_chord_active = 0;
            continue;
        }
        if (raw == 0x1D) { keyboard_ctrl = 1; keyboard_extended = 0; continue; }
        if (raw == 0x9D) {
            keyboard_ctrl = 0;
            keyboard_extended = 0;
            continue;
        }
        if (raw == 0x3A) { keyboard_caps ^= 1; continue; }
        if (raw & 0x80) { keyboard_extended = 0; continue; }
        if (keyboard_extended) {
            extended_key_action(raw);
        } else {
            app_before = active_app;
            key_action(raw);
            ascii = keyboard_ascii(raw);
            if (ascii && active_app == app_before && !keyboard_ctrl) key_ascii_action(ascii);
        }
        keyboard_extended = 0;
    }
}

static void game_update(void) {
    int i;
    if (active_app != APP_SNAKE && active_app != APP_SHOOTER) {
        game_delay = 0;
        return;
    }
    if (++game_delay < 1500000) return;
    game_delay = 0;

    if (active_app == APP_SNAKE && snake_alive) {
        int head_x = snake_x[0];
        int head_y = snake_y[0];
        if (snake_direction == 0) head_y--;
        else if (snake_direction == 1) head_x++;
        else if (snake_direction == 2) head_y++;
        else head_x--;
        if (head_x < 0 || head_x >= 24 || head_y < 0 || head_y >= 10) {
            snake_alive = 0;
            screen_dirty = 1;
            return;
        }
        for (i = 0; i < snake_length; i++) {
            if (snake_x[i] == head_x && snake_y[i] == head_y) {
                snake_alive = 0;
                screen_dirty = 1;
                return;
            }
        }
        if (head_x == snake_food_x && head_y == snake_food_y && snake_length < 32) {
            snake_length++;
            snake_score++;
            snake_food_x = (u8)((ticks / 97 + snake_score * 7) % 24);
            snake_food_y = (u8)((ticks / 53 + snake_score * 3) % 10);
        }
        for (i = snake_length - 1; i > 0; i--) {
            snake_x[i] = snake_x[i - 1];
            snake_y[i] = snake_y[i - 1];
        }
        snake_x[0] = (u8)head_x;
        snake_y[0] = (u8)head_y;
        screen_dirty = 1;
    } else if (active_app == APP_SHOOTER) {
        static u8 target_direction = 1;
        if (target_direction) {
            if (shooter_target_x >= 232) target_direction = 0;
            else shooter_target_x = (u8)(shooter_target_x + 2);
        } else {
            if (shooter_target_x <= 12) target_direction = 1;
            else shooter_target_x = (u8)(shooter_target_x - 2);
        }
        if (shooter_bullet_active) {
            if (shooter_bullet_y > 46) shooter_bullet_y = (u8)(shooter_bullet_y - 3);
            else shooter_bullet_active = 0;
            if (shooter_bullet_active &&
                shooter_bullet_x + 3 >= shooter_target_x &&
                shooter_bullet_x <= shooter_target_x + 14 &&
                shooter_bullet_y <= shooter_target_y + 14 &&
                shooter_bullet_y + 6 >= shooter_target_y) {
                shooter_bullet_active = 0;
                shooter_score++;
                shooter_target_x = (u8)((ticks / 149 + shooter_score * 31) % 225 + 10);
                shooter_target_y = (u8)(56 + (shooter_score % 4) * 12);
            }
        }
        screen_dirty = 1;
    }
}

extern "C" void kernel_main(void) {
    memset8(FB, 0, WIDTH * HEIGHT);
    fs_init();
    for (u8 i = 0; i < snake_length; i++) {
        snake_x[i] = (u8)(10 - i);
        snake_y[i] = 5;
    }
    browser_load();
    mouse_init();
    network_init();
    for (;;) {
        keyboard_poll();
        mouse_poll();
        network_poll();
        game_update();
        if (screen_dirty) {
            render();
            screen_dirty = 0;
        }
        draw_cursor();
        ticks++;
    }
}
