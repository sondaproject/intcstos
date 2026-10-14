// intcstOS kernel version z2-b (beta, check for bugs please.) added RAM filesystem, file manager, notepad, and more simple linux commands
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
#error "You are not using a cross-compiler, you will most certainly run into trouble"
#endif

// Multiboot Definition
typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed)) multiboot_info_t;

// Framebuffer Setup
static uint32_t* framebuffer = NULL;
static uint32_t screen_width = 0;
static uint32_t screen_height = 0;
static uint32_t screen_pitch = 0;

// Color Palette 
#define color_desktop_bg 0x001b4d3e
#define color_taskbar    0x002d3748
#define color_button     0x003182ce
#define color_menu_bg    0x001a202c
#define color_window_hdr 0x002b6cb0
#define color_close_btn  0x00e53e3e
#define color_white      0x00ffffff
#define color_black      0x00000000
#define color_green      0x0000ff00
#define color_dark_gray  0x004a5568
#define color_light_gray 0x00718096
#define color_orange     0x00dd6b20
#define color_yellow     0x00ecc94b
#define color_teal       0x00319795

// Port I/O Helpers
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0x00);
}

#include "font.c"

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= screen_width || y >= screen_height) return;
    framebuffer[(y * (screen_pitch / 4)) + x] = color;
}

static inline uint32_t get_pixel(uint32_t x, uint32_t y) {
    if (x >= screen_width || y >= screen_height) return 0;
    return framebuffer[(y * (screen_pitch / 4)) + x];
}

void draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j++) {
            put_pixel(x + j, y + i, color);
        }
    }
}

void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color) {
    uint8_t index = (uint8_t)c;
    if (index >= 128) return;
    for (int row = 0; row < 16; row++) {
        uint8_t line = font_8x16[index][row];
        for (int col = 0; col < 8; col++) {
            if (line & (1 << (7 - col))) put_pixel(x + col, y + row, fg_color);
        }
    }
}

void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg_color) {
    uint32_t cur_x = x;
    while (*str) {
        if (*str == '\n') { cur_x = x; y += 16; } 
        else { draw_char(*str, cur_x, y, fg_color); cur_x += 8; }
        str++;
    }
}

//ramfs engine. stores files in ram WARNING, reboot will cause yor file to get deleted
#define MAX_VFS_NODES 64
#define MAX_FILENAME_LEN 32
#define MAX_FILE_SIZE 512

typedef struct {
    char name[MAX_FILENAME_LEN];
    bool is_dir;
    bool used;
    int parent;
    uint32_t size;
    char content[MAX_FILE_SIZE];
} vfs_node_t;

static vfs_node_t vfs_nodes[MAX_VFS_NODES];
static int current_dir_idx = 0; // Root directory is 0

static bool strcmp_exact(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return (*a == *b);
}

static void strcpy_safe(char* dest, const char* src, uint32_t max_len) {
    uint32_t i = 0;
    while (src[i] != '\0' && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void vfs_init(void) {
    for (int i = 0; i < MAX_VFS_NODES; i++) {
        vfs_nodes[i].used = false;
        vfs_nodes[i].is_dir = false;
        vfs_nodes[i].parent = -1;
        vfs_nodes[i].size = 0;
        vfs_nodes[i].content[0] = '\0';
    }
    // the root dir (/)
    vfs_nodes[0].used = true;
    vfs_nodes[0].is_dir = true;
    vfs_nodes[0].parent = -1;
    strcpy_safe(vfs_nodes[0].name, "/", MAX_FILENAME_LEN);
    current_dir_idx = 0;
}

int vfs_find_child(int parent_idx, const char* name) {
    for (int i = 0; i < MAX_VFS_NODES; i++) {
        if (vfs_nodes[i].used && vfs_nodes[i].parent == parent_idx) {
            if (strcmp_exact(vfs_nodes[i].name, name)) return i;
        }
    }
    return -1;
}

int vfs_create_node(int parent_idx, const char* name, bool is_dir) {
    if (vfs_find_child(parent_idx, name) != -1) return -1; // Already exists
    for (int i = 0; i < MAX_VFS_NODES; i++) {
        if (!vfs_nodes[i].used) {
            vfs_nodes[i].used = true;
            vfs_nodes[i].is_dir = is_dir;
            vfs_nodes[i].parent = parent_idx;
            vfs_nodes[i].size = 0;
            vfs_nodes[i].content[0] = '\0';
            strcpy_safe(vfs_nodes[i].name, name, MAX_FILENAME_LEN);
            return i;
        }
    }
    return -1;
}

void vfs_get_path(int node_idx, char* out_buf) {
    if (node_idx == 0) {
        strcpy_safe(out_buf, "/", MAX_FILENAME_LEN);
        return;
    }
    char temp[128] = {0};
    int current = node_idx;
    out_buf[0] = '\0';

    while (current > 0) {
        char seg[MAX_FILENAME_LEN + 2] = "/";
        uint32_t idx = 1;
        for (uint32_t i = 0; vfs_nodes[current].name[i] != '\0'; i++) {
            seg[idx++] = vfs_nodes[current].name[i];
        }
        seg[idx] = '\0';

        // Prepend segment
        char prev_temp[128];
        strcpy_safe(prev_temp, temp, 128);
        strcpy_safe(temp, seg, 128);
        
        uint32_t temp_len = idx;
        for (uint32_t j = 0; prev_temp[j] != '\0' && temp_len < 127; j++) {
            temp[temp_len++] = prev_temp[j];
        }
        temp[temp_len] = '\0';

        current = vfs_nodes[current].parent;
    }
    strcpy_safe(out_buf, temp, 128);
}

//application sates, tells the kernel to "draw" the window and whats inside
enum WindowFocus { FOCUS_NONE, FOCUS_TERM, FOCUS_CALC, FOCUS_NOTEPAD, FOCUS_FM };
static enum WindowFocus focused_window = FOCUS_TERM;

static bool start_menu_open = false;

static bool window_open = false;
static int win_x = 100, win_y = 60, win_w = 520, win_h = 350;

static bool calc_open = false;
static int calc_x = 220, calc_y = 90, calc_w = 210, calc_h = 270;
static char calc_disp[16] = "0";
static int calc_disp_len = 1;
static int32_t calc_operand = 0;
static char calc_op = 0;
static bool calc_clear_on_next = false;

static bool notepad_open = false;
static int np_x = 180, np_y = 80, np_w = 420, np_h = 300;
static int np_file_idx = -1;
static char np_filename[MAX_FILENAME_LEN] = "untitled.txt";
static char np_buffer[MAX_FILE_SIZE] = {0};
static uint32_t np_buf_len = 0;

static bool fm_open = false;
static int fm_x = 140, fm_y = 100, fm_w = 440, fm_h = 290;
static int fm_current_dir = 0;

// window dragging function
enum DragTarget { DRAG_NONE, DRAG_TERM, DRAG_CALC, DRAG_NOTEPAD, DRAG_FM };
static enum DragTarget current_drag = DRAG_NONE;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static bool prev_left_click = false;

#define TERM_MAX_LINES 12
#define TERM_LINE_LEN 60
static char term_lines[TERM_MAX_LINES][TERM_LINE_LEN];
static uint32_t term_line_count = 0;

#define kb_buf_max 32
static char term_input[kb_buf_max] = {0};
static uint32_t term_input_len = 0;

//keyboard map
static const char scancode_ascii[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

//function to print text on the terminal
void term_print(const char* msg) {
    if (term_line_count >= TERM_MAX_LINES) {
        for (uint32_t i = 1; i < TERM_MAX_LINES; i++) {
            for (uint32_t j = 0; j < TERM_LINE_LEN; j++) {
                term_lines[i - 1][j] = term_lines[i][j];
            }
        }
        term_line_count = TERM_MAX_LINES - 1;
    }

    uint32_t idx = 0;
    while (msg[idx] != '\0' && idx < TERM_LINE_LEN - 1) {
        term_lines[term_line_count][idx] = msg[idx];
        idx++;
    }
    term_lines[term_line_count][idx] = '\0';
    term_line_count++;
}

void term_clear(void) {
    term_line_count = 0;
    for (uint32_t i = 0; i < TERM_MAX_LINES; i++) {
        term_lines[i][0] = '\0';
    }
}

//notepad functons
void open_notepad_file(const char* filename) {
    int node = vfs_find_child(current_dir_idx, filename);
    if (node == -1) {
        node = vfs_create_node(current_dir_idx, filename, false);
    }
    
    if (node != -1 && !vfs_nodes[node].is_dir) {
        np_file_idx = node;
        strcpy_safe(np_filename, vfs_nodes[node].name, MAX_FILENAME_LEN);
        strcpy_safe(np_buffer, vfs_nodes[node].content, MAX_FILE_SIZE);
        np_buf_len = 0;
        while (np_buffer[np_buf_len] != '\0') np_buf_len++;
        notepad_open = true;
        focused_window = FOCUS_NOTEPAD;
    } else {
        term_print("Error: Target is a directory or disk full.");
    }
}

void execute_command(const char* cmd) {
    char echo_buf[68] = "int> ";
    uint32_t idx = 5;
    uint32_t c_idx = 0;
    while (cmd[c_idx] != '\0' && idx < 67) {
        echo_buf[idx++] = cmd[c_idx++];
    }
    echo_buf[idx] = '\0';
    term_print(echo_buf);

    // Extract Command Name and First Parameter
    char name[32] = {0};
    char arg[32] = {0};
    uint32_t p = 0, np = 0, ap = 0;

    while (cmd[p] == ' ') p++;
    while (cmd[p] != ' ' && cmd[p] != '\0' && np < 31) name[np++] = cmd[p++];
    name[np] = '\0';
    while (cmd[p] == ' ') p++;
    while (cmd[p] != '\0' && ap < 31) arg[ap++] = cmd[p++];
    arg[ap] = '\0';

    if (strcmp_exact(name, "help")) {
        term_print("System Commands:");
        term_print("  ls, mkdir, touch, cd, pwd, cat, note");
        term_print("  calc, fetch, about, clear, reboot");
    } else if (strcmp_exact(name, "ls")) {
        term_print("Directory Contents:");
        for (int i = 0; i < MAX_VFS_NODES; i++) {
            if (vfs_nodes[i].used && vfs_nodes[i].parent == current_dir_idx) {
                char item_line[48] = "  ";
                uint32_t lp = 2;
                if (vfs_nodes[i].is_dir) {
                    item_line[lp++] = '['; item_line[lp++] = 'D'; item_line[lp++] = 'I'; item_line[lp++] = 'R'; item_line[lp++] = ']'; item_line[lp++] = ' ';
                }
                for (uint32_t k = 0; vfs_nodes[i].name[k] != '\0' && lp < 45; k++) {
                    item_line[lp++] = vfs_nodes[i].name[k];
                }
                item_line[lp] = '\0';
                term_print(item_line);
            }
        }
    } else if (strcmp_exact(name, "mkdir")) {
        if (arg[0] == '\0') term_print("Usage: mkdir <dirname>");
        else if (vfs_create_node(current_dir_idx, arg, true) == -1) term_print("Error creating directory.");
    } else if (strcmp_exact(name, "touch")) {
        if (arg[0] == '\0') term_print("Usage: touch <filename>");
        else if (vfs_create_node(current_dir_idx, arg, false) == -1) term_print("Error creating file.");
    } else if (strcmp_exact(name, "cd")) {
        if (arg[0] == '\0' || strcmp_exact(arg, "/")) {
            current_dir_idx = 0;
        } else if (strcmp_exact(arg, "..")) {
            if (vfs_nodes[current_dir_idx].parent != -1) {
                current_dir_idx = vfs_nodes[current_dir_idx].parent;
            }
        } else {
            int target = vfs_find_child(current_dir_idx, arg);
            if (target != -1 && vfs_nodes[target].is_dir) {
                current_dir_idx = target;
            } else {
                term_print("Directory not found.");
            }
        }
    } else if (strcmp_exact(name, "pwd")) {
        char path[128];
        vfs_get_path(current_dir_idx, path);
        term_print(path);
    } else if (strcmp_exact(name, "cat")) {
        if (arg[0] == '\0') term_print("Usage: cat <filename>");
        else {
            int target = vfs_find_child(current_dir_idx, arg);
            if (target != -1 && !vfs_nodes[target].is_dir) {
                term_print(vfs_nodes[target].content);
            } else {
                term_print("File not found.");
            }
        }
    } else if (strcmp_exact(name, "note")) {
        if (arg[0] == '\0') open_notepad_file("untitled.txt");
        else open_notepad_file(arg);
    } else if (strcmp_exact(name, "clear")) {
        term_clear();
    } else if (strcmp_exact(name, "about")) {
        term_print("intcstOS z1.2 - 32-bit GUI Kernel by SondaProject");
    } else if (strcmp_exact(name, "fetch")) {
        term_print(" ___ _   _ _____    ");
        term_print("|_ _| \\ | |_   _|   OS: intcstOS i686-32");
        term_print(" | ||  \\| | | |     Version: z2-b (beta)");
        term_print(" | || |\\  | | |     Display Engine: VBE");
        term_print("|___|_| \\_| |_|  ");
    } else if (strcmp_exact(name, "calc")) {
        calc_open = true;
        focused_window = FOCUS_CALC;
    } else if (strcmp_exact(name, "reboot")) {
        outb(0x64, 0xfe);
    } else if (cmd[0] != '\0') {
        term_print("Unknown command. Type 'help'.");
    }
}

//draws the desktop icons
void draw_desktop_icons(void) {
    //terminal
    uint32_t x = 30, y = 30;
    draw_rect(x, y, 48, 40, color_black);
    draw_rect(x + 2, y + 2, 44, 36, color_dark_gray);
    draw_string(">", x + 8, y + 12, color_green);
    draw_string("_", x + 20, y + 12, color_white);
    draw_string("Terminal", x - 8, y + 46, color_white);

    //calculator
    x = 30; y = 105;
    draw_rect(x, y, 48, 40, color_black);
    draw_rect(x + 2, y + 2, 44, 36, color_light_gray);
    draw_string("12", x + 8, y + 6, color_white);
    draw_string("+=", x + 8, y + 20, color_orange);
    draw_string("Calc", x + 8, y + 46, color_white);

    //file manager
    x = 30; y = 180;
    draw_rect(x, y, 48, 40, color_black);
    draw_rect(x + 2, y + 2, 44, 36, color_yellow);
    draw_string("DIR", x + 10, y + 12, color_black);
    draw_string("Files", x + 4, y + 46, color_white);

    //notepad
    x = 30; y = 255;
    draw_rect(x, y, 48, 40, color_black);
    draw_rect(x + 2, y + 2, 44, 36, color_white);
    draw_string("TXT", x + 10, y + 12, color_dark_gray);
    draw_string("Notepad", x - 4, y + 46, color_white);
}

//draw the terminal window
void draw_terminal_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!window_open) return;

    //background
    draw_rect(x, y, w, h, color_black);
    draw_rect(x, y, w, 24, (focused_window == FOCUS_TERM) ? color_window_hdr : color_dark_gray);

    //close button for windows
    draw_rect(x + w - 24, y + 2, 20, 20, color_close_btn);
    draw_string("X", x + w - 18, y + 4, color_white);

    //window title
    draw_string("Terminal", x + 8, y + 4, color_white);

    uint32_t start_y = y + 32;
    for (uint32_t i = 0; i < term_line_count; i++) {
        draw_string(term_lines[i], x + 12, start_y + (i * 20), color_white);
    }

    //terminal shell
    uint32_t prompt_y = start_y + (term_line_count * 20);
    if (prompt_y <= y + h - 28) {
        const char* prompt = "voidsh> ";
        uint32_t prompt_x = x + 12;            // Shifted from x + 9 to align with term_lines
        uint32_t input_x = prompt_x + (8 * 8);  // 8 chars * 8px = x + 76 (prevents overlap)

        draw_string(prompt, prompt_x, prompt_y, color_white);
        draw_string(term_input, input_x, prompt_y, color_green);

        if (focused_window == FOCUS_TERM) {
            draw_rect(input_x + (term_input_len * 8), prompt_y, 8, 16, color_green);
        }
    }
}

#include "calc.c"

//draws the notepad window
void draw_notepad_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!notepad_open) return;

    draw_rect(x, y, w, h, color_white);
    draw_rect(x, y, w, 24, (focused_window == FOCUS_NOTEPAD) ? color_teal : color_dark_gray);

    draw_rect(x + w - 24, y + 2, 20, 20, color_close_btn);
    draw_string("X", x + w - 18, y + 4, color_white);

    char title_buf[48] = "Notepad - ";
    strcpy_safe(title_buf + 10, np_filename, 32);
    draw_string(title_buf, x + 8, y + 4, color_white);

    uint32_t cur_x = x + 8;
    uint32_t cur_y = y + 32;
    
    for (uint32_t i = 0; i < np_buf_len; i++) {
        char c = np_buffer[i];
        if (c == '\n') {
            cur_x = x + 8;
            cur_y += 18;
        } else {
            draw_char(c, cur_x, cur_y, color_black);
            cur_x += 8;
            if (cur_x >= x + w - 16) {
                cur_x = x + 8;
                cur_y += 18;
            }
        }
    }

    //cursor
    if (focused_window == FOCUS_NOTEPAD) {
        draw_rect(cur_x, cur_y, 2, 16, color_black);
    }
}

//same thing but with the file manager
void draw_file_manager_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fm_open) return;

    draw_rect(x, y, w, h, color_menu_bg);
    draw_rect(x, y, w, 24, (focused_window == FOCUS_FM) ? color_button : color_dark_gray);

    draw_rect(x + w - 24, y + 2, 20, 20, color_close_btn);
    draw_string("X", x + w - 18, y + 4, color_white);

    char path_buf[128] = "Files: ";
    vfs_get_path(fm_current_dir, path_buf + 7);
    draw_string(path_buf, x + 8, y + 4, color_white);

    draw_rect(x + 8, y + 30, 48, 22, color_dark_gray);
    draw_string("[..]", x + 12, y + 34, color_white);

    uint32_t item_y = y + 60;
    for (int i = 0; i < MAX_VFS_NODES; i++) {
        if (vfs_nodes[i].used && vfs_nodes[i].parent == fm_current_dir) {
            if (item_y + 20 >= y + h) break;

            if (vfs_nodes[i].is_dir) {
                draw_rect(x + 12, item_y, 16, 14, color_yellow);
                draw_string(vfs_nodes[i].name, x + 36, item_y, color_white);
            } else {
                draw_rect(x + 12, item_y, 16, 14, color_light_gray);
                draw_string(vfs_nodes[i].name, x + 36, item_y, color_white);
            }
            item_y += 22;
        }
    }
}

//draws the start menu
void draw_start_menu(void) {
    if (!start_menu_open) return;
    
    uint32_t menu_x = 10;
    uint32_t menu_y = screen_height - 270;
    uint32_t menu_w = 160;
    uint32_t menu_h = 228;

    draw_rect(menu_x, menu_y, menu_w, menu_h, color_menu_bg);
    draw_rect(menu_x, menu_y, menu_w, 2, color_button);

    draw_string("> Terminal", menu_x + 12, menu_y + 12, color_white);
    draw_string("> Calculator", menu_x + 12, menu_y + 48, color_white);
    draw_string("> File Manager", menu_x + 12, menu_y + 84, color_white);
    draw_string("> Notepad", menu_x + 12, menu_y + 120, color_white);
    draw_string("> Clear Screen", menu_x + 12, menu_y + 156, color_white);
    draw_string("> Reboot System", menu_x + 12, menu_y + 192, color_white);
}

void ui_render_desktop(void) {
    draw_rect(0, 0, screen_width, screen_height, color_desktop_bg);
    draw_desktop_icons();
    
    draw_terminal_window(win_x, win_y, win_w, win_h);
    draw_calculator_window(calc_x, calc_y, calc_w, calc_h);
    draw_notepad_window(np_x, np_y, np_w, np_h);
    draw_file_manager_window(fm_x, fm_y, fm_w, fm_h);

    // Taskbar
    draw_rect(0, screen_height - 40, screen_width, 40, color_taskbar);
    draw_rect(10, screen_height - 32, 80, 24, color_button);
    draw_string("Start", 28, screen_height - 28, color_white);

    draw_start_menu();
}

//mouse cursor, fully customizable.
#define cursor_w 12
#define cursor_h 19

static const char* cursor_sprite[cursor_h] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX   X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
    "            "
};

static int mouse_x = 400;
static int mouse_y = 300;
static uint32_t mouse_backbuffer[cursor_w * cursor_h];

void mouse_save_backbuffer(int x, int y) {
    for (int cy = 0; cy < cursor_h; cy++) {
        for (int cx = 0; cx < cursor_w; cx++) {
            mouse_backbuffer[cy * cursor_w + cx] = get_pixel(x + cx, y + cy);
        }
    }
}

void mouse_restore_backbuffer(int x, int y) {
    for (int cy = 0; cy < cursor_h; cy++) {
        for (int cx = 0; cx < cursor_w; cx++) {
            put_pixel(x + cx, y + cy, mouse_backbuffer[cy * cursor_w + cx]);
        }
    }
}

void mouse_draw_sprite(int x, int y) {
    for (int cy = 0; cy < cursor_h; cy++) {
        for (int cx = 0; cx < cursor_w; cx++) {
            char pixel = cursor_sprite[cy][cx];
            if (pixel == 'X') put_pixel(x + cx, y + cy, color_black);
            else if (pixel == '.') put_pixel(x + cx, y + cy, color_white);
        }
    }
}

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 10000;
    if (type == 0) {
        while (timeout--) {
            io_wait();
            if (inb(0x64) & 1) return;
        }
    } else {
        while (timeout--) {
            io_wait();
            if (!(inb(0x64) & 2)) return;
        }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xd4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void ps2_mouse_init(void) {
    uint8_t status;
    mouse_wait(1); outb(0x64, 0xa8); 
    mouse_wait(1); outb(0x64, 0x20);
    status = mouse_read();
    status |= 0x02; status &= ~0x20;  
    mouse_wait(1); outb(0x64, 0x60);
    mouse_wait(1); outb(0x60, status);
    mouse_write(0xf6); mouse_read();
    mouse_write(0xf4); mouse_read();
    while (inb(0x64) & 1) { inb(0x60); io_wait(); } 

    mouse_save_backbuffer(mouse_x, mouse_y);
    mouse_draw_sprite(mouse_x, mouse_y);
}

static bool mouse_read_byte(uint8_t* out_byte) {
    uint8_t status = inb(0x64);
    if ((status & 0x01) && (status & 0x20)) {
        *out_byte = inb(0x60);
        return true;
    }
    return false;
}

//mouse click events, for example when you click on an icon
void handle_mouse_events(int x, int y, bool left_down) {
    bool click_event = left_down && !prev_left_click;
    bool needs_redraw = false;

    if (click_event && x >= 20 && x <= 80) {
        if (y >= 20 && y <= 80) {
            window_open = true; focused_window = FOCUS_TERM; needs_redraw = true;
        } else if (y >= 95 && y <= 155) {
            calc_open = true; focused_window = FOCUS_CALC; needs_redraw = true;
        } else if (y >= 170 && y <= 230) {
            fm_open = true; focused_window = FOCUS_FM; needs_redraw = true;
        } else if (y >= 245 && y <= 305) {
            open_notepad_file("untitled.txt"); needs_redraw = true;
        }
    }

    else if (click_event && x >= 10 && x <= 90 && y >= (int)screen_height - 32 && y <= (int)screen_height - 8) {
        start_menu_open = !start_menu_open;
        needs_redraw = true;
    }

    else if (start_menu_open && click_event) {
        uint32_t menu_x = 10;
        uint32_t menu_y = screen_height - 270;
        if (x >= (int)menu_x && x <= (int)(menu_x + 160)) {
            if (y >= (int)(menu_y + 10) && y <= (int)(menu_y + 40)) {
                window_open = true; focused_window = FOCUS_TERM;
            } else if (y >= (int)(menu_y + 40) && y <= (int)(menu_y + 70)) {
                calc_open = true; focused_window = FOCUS_CALC;
            } else if (y >= (int)(menu_y + 70) && y <= (int)(menu_y + 110)) {
                fm_open = true; focused_window = FOCUS_FM;
            } else if (y >= (int)(menu_y + 110) && y <= (int)(menu_y + 140)) {
                open_notepad_file("untitled.txt");
            } else if (y >= (int)(menu_y + 140) && y <= (int)(menu_y + 180)) {
                term_clear();
            } else if (y >= (int)(menu_y + 180) && y <= (int)(menu_y + 220)) {
                outb(0x64, 0xfe);
            }
        }
        start_menu_open = false;
        needs_redraw = true;
    }

    else if (window_open && click_event && (x >= win_x + win_w - 24 && x <= win_x + win_w - 4 && y >= win_y + 2 && y <= win_y + 22)) {
        window_open = false; needs_redraw = true;
    } else if (calc_open && click_event && (x >= calc_x + calc_w - 24 && x <= calc_x + calc_w - 4 && y >= calc_y + 2 && y <= calc_y + 22)) {
        calc_open = false; needs_redraw = true;
    } else if (notepad_open && click_event && (x >= np_x + np_w - 24 && x <= np_x + np_w - 4 && y >= np_y + 2 && y <= np_y + 22)) {
        notepad_open = false; needs_redraw = true;
    } else if (fm_open && click_event && (x >= fm_x + fm_w - 24 && x <= fm_x + fm_w - 4 && y >= fm_y + 2 && y <= fm_y + 22)) {
        fm_open = false; needs_redraw = true;
    }

    else if (fm_open && click_event && (x >= fm_x + 8 && x <= fm_x + 56 && y >= fm_y + 30 && y <= fm_y + 52)) {
        if (vfs_nodes[fm_current_dir].parent != -1) {
            fm_current_dir = vfs_nodes[fm_current_dir].parent;
            needs_redraw = true;
        }
    }

    else if (fm_open && click_event && (x >= fm_x + 12 && x <= fm_x + fm_w - 12 && y >= fm_y + 60)) {
        uint32_t item_y = fm_y + 60;
        for (int i = 0; i < MAX_VFS_NODES; i++) {
            if (vfs_nodes[i].used && vfs_nodes[i].parent == fm_current_dir) {
                if (y >= (int)item_y && y <= (int)(item_y + 20)) {
                    if (vfs_nodes[i].is_dir) {
                        fm_current_dir = i;
                    } else {
                        open_notepad_file(vfs_nodes[i].name);
                    }
                    needs_redraw = true;
                    break;
                }
                item_y += 22;
            }
        }
    }

    if (click_event && current_drag == DRAG_NONE) {
        if (notepad_open && x >= np_x && x <= np_x + np_w && y >= np_y && y <= np_y + np_h) {
            focused_window = FOCUS_NOTEPAD;
            if (y <= np_y + 24) { current_drag = DRAG_NOTEPAD; drag_offset_x = x - np_x; drag_offset_y = y - np_y; }
            needs_redraw = true;
        } else if (fm_open && x >= fm_x && x <= fm_x + fm_w && y >= fm_y && y <= fm_y + fm_h) {
            focused_window = FOCUS_FM;
            if (y <= fm_y + 24) { current_drag = DRAG_FM; drag_offset_x = x - fm_x; drag_offset_y = y - fm_y; }
            needs_redraw = true;
        } else if (calc_open && x >= calc_x && x <= calc_x + calc_w && y >= calc_y && y <= calc_y + calc_h) {
            focused_window = FOCUS_CALC;
            if (y <= calc_y + 24) { current_drag = DRAG_CALC; drag_offset_x = x - calc_x; drag_offset_y = y - calc_y; }
            needs_redraw = true;
        } else if (window_open && x >= win_x && x <= win_x + win_w && y >= win_y && y <= win_y + win_h) {
            focused_window = FOCUS_TERM;
            if (y <= win_y + 24) { current_drag = DRAG_TERM; drag_offset_x = x - win_x; drag_offset_y = y - win_y; }
            needs_redraw = true;
        }
    }

    if (left_down && current_drag != DRAG_NONE) {
        if (current_drag == DRAG_TERM) { win_x = x - drag_offset_x; win_y = y - drag_offset_y; needs_redraw = true; }
        else if (current_drag == DRAG_CALC) { calc_x = x - drag_offset_x; calc_y = y - drag_offset_y; needs_redraw = true; }
        else if (current_drag == DRAG_NOTEPAD) { np_x = x - drag_offset_x; np_y = y - drag_offset_y; needs_redraw = true; }
        else if (current_drag == DRAG_FM) { fm_x = x - drag_offset_x; fm_y = y - drag_offset_y; needs_redraw = true; }
    }

    if (!left_down) current_drag = DRAG_NONE;
    if (needs_redraw) ui_render_desktop();
    prev_left_click = left_down;
}

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

void mouse_poll_update(void) {
    uint8_t b;
    while (mouse_read_byte(&b)) {
        if (mouse_cycle == 0) {
            if (b & 0x08) { mouse_packet[0] = b; mouse_cycle = 1; }
        } else if (mouse_cycle == 1) {
            mouse_packet[1] = b; mouse_cycle = 2;
        } else if (mouse_cycle == 2) {
            mouse_packet[2] = b; mouse_cycle = 0;

            int rel_x = (int8_t)mouse_packet[1];
            int rel_y = (int8_t)mouse_packet[2];
            bool left_click = (mouse_packet[0] & 0x01);

            mouse_restore_backbuffer(mouse_x, mouse_y);
            mouse_x += rel_x; mouse_y -= rel_y; 

            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= (int)screen_width - cursor_w)  mouse_x = screen_width - cursor_w;
            if (mouse_y >= (int)screen_height - cursor_h) mouse_y = screen_height - cursor_h;

            handle_mouse_events(mouse_x, mouse_y, left_click);
            mouse_save_backbuffer(mouse_x, mouse_y);
            mouse_draw_sprite(mouse_x, mouse_y);
        }
    }
}

/* Keyboard Dispatcher Routine */
void keyboard_poll_update(void) {
    uint8_t status = inb(0x64);
    if ((status & 0x01) && !(status & 0x20)) {
        uint8_t scancode = inb(0x60);

        if (!(scancode & 0x80)) {
            char key = scancode_ascii[scancode];
            bool char_added = false;

            // Route Input Based on Focused Application
            if (focused_window == FOCUS_NOTEPAD && notepad_open) {
                if (key == '\b') {
                    if (np_buf_len > 0) {
                        np_buf_len--;
                        np_buffer[np_buf_len] = '\0';
                        char_added = true;
                    }
                } else if (key == '\n' || (key >= ' ' && key <= '~')) {
                    if (np_buf_len < MAX_FILE_SIZE - 1) {
                        np_buffer[np_buf_len++] = key;
                        np_buffer[np_buf_len] = '\0';
                        char_added = true;
                    }
                }

                // Save Back to RAMFS Live Node
                if (np_file_idx != -1 && char_added) {
                    strcpy_safe(vfs_nodes[np_file_idx].content, np_buffer, MAX_FILE_SIZE);
                    vfs_nodes[np_file_idx].size = np_buf_len;
                }
            } else if (focused_window == FOCUS_TERM && window_open) {
                if (key == '\b') {
                    if (term_input_len > 0) {
                        term_input_len--;
                        term_input[term_input_len] = '\0';
                        char_added = true;
                    }
                } else if (key == '\n') {
                    execute_command(term_input);
                    term_input_len = 0;
                    term_input[0] = '\0';
                    char_added = true;
                } else if (key >= ' ' && key <= '~') {
                    if (term_input_len < kb_buf_max - 1) {
                        term_input[term_input_len++] = key;
                        term_input[term_input_len] = '\0';
                        char_added = true;
                    }
                }
            }

            if (char_added) {
                mouse_restore_backbuffer(mouse_x, mouse_y);
                ui_render_desktop();
                mouse_save_backbuffer(mouse_x, mouse_y);
                mouse_draw_sprite(mouse_x, mouse_y);
            }
        }
    }
}

//kernel start
void kernel_main(uint32_t magic, multiboot_info_t* mb_info) {
    if (magic != 0x2badb002 || !(mb_info->flags & (1 << 12))) return;

    framebuffer = (uint32_t*)(uintptr_t)mb_info->framebuffer_addr;
    screen_width = mb_info->framebuffer_width;
    screen_height = mb_info->framebuffer_height;
    screen_pitch = mb_info->framebuffer_pitch;

    // Initialize VFS System
    vfs_init();

    // Default Seed Files
    vfs_create_node(0, "main", true);
    int docs_dir = vfs_find_child(0, "main");
    int note_file = vfs_create_node(docs_dir, "welcome.txt", false);
    if (note_file != -1) {
        strcpy_safe(vfs_nodes[note_file].content, "Welcome to INTcstOS", MAX_FILE_SIZE);
        vfs_nodes[note_file].size = 26;
    }

    term_clear();
    term_print("intcstOS Kernel z2 Started.");
    term_print("drivers succesfully initialized. Type 'help' for commands.");

    ui_render_desktop();
    ps2_mouse_init();

    while (1) {
        mouse_poll_update();
        keyboard_poll_update();
    }
}
