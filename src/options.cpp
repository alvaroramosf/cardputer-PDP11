#include "options.h"
#include <Preferences.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <SD.h>
#include "kb11.h"

extern KB11 cpu;

EmulatorOptions current_options;
Preferences preferences;

bool request_soft_reset = false;
bool request_load_snapshot = false;
String snapshot_to_load = "";
int soft_reset_disk_idx = 0;

void loadOptions() {
    preferences.begin("pdp11", false);
    for (int i = 0; i < 4; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rk_disk_%d", i);
        current_options.rk_disks[i] = preferences.getInt(key, (i == 0) ? 0 : -1);
        snprintf(key, sizeof(key), "rl_disk_%d", i);
        current_options.rl_disks[i] = preferences.getInt(key, -1);
    }
    current_options.term_color  = (TermColor)preferences.getInt("term_color", COLOR_GREEN);
    current_options.brightness  = preferences.getInt("brightness", 200);
    current_options.cpu_model   = (CpuModel)preferences.getInt("cpu_model", CPU_PDP1140);
    current_options.boot_device = (BootDevice)preferences.getInt("boot_device", BOOT_RK);
    current_options.led_enabled = preferences.getBool("led_enabled", true);
    current_options.font_size   = preferences.getInt("font_size", 1);
    preferences.end();
}

void saveOptions() {
    preferences.begin("pdp11", false);
    for (int i = 0; i < 4; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rk_disk_%d", i);
        preferences.putInt(key, current_options.rk_disks[i]);
        snprintf(key, sizeof(key), "rl_disk_%d", i);
        preferences.putInt(key, current_options.rl_disks[i]);
    }
    preferences.putInt("term_color", current_options.term_color);
    preferences.putInt("brightness", current_options.brightness);
    preferences.putInt("cpu_model",  current_options.cpu_model);
    preferences.putInt("boot_device", current_options.boot_device);
    preferences.putBool("led_enabled", current_options.led_enabled);
    preferences.putInt("font_size", current_options.font_size);
    preferences.end();
}

void applyOptions() {
    M5Cardputer.Display.setBrightness(current_options.brightness);
    extern void update_canvas_colors();
    update_canvas_colors();
    extern void apply_canvas_font_size();
    apply_canvas_font_size();
}

static void waitForKeyRelease() {
    while (M5Cardputer.Keyboard.isPressed()) {
        M5Cardputer.update();
        delay(10);
    }
}

static uint16_t getMenuColor() {
    if (current_options.term_color == COLOR_AMBER) return 0xFFB000;
    if (current_options.term_color == COLOR_WHITE) return TFT_WHITE;
    if (current_options.term_color == COLOR_PAPER) return BLACK;
    return TFT_GREEN;
}

static uint16_t getMenuBgColor() {
    if (current_options.term_color == COLOR_PAPER) return 0xFFFFCC;
    return BLACK;
}

// Menu system helpers
static void drawMenuHeader(const char* title) {
    uint16_t fg = getMenuColor();
    uint16_t bg = getMenuBgColor();
    M5Cardputer.Display.fillScreen(bg);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setFont(&fonts::Font0); // Default font for menus
    
    M5Cardputer.Display.setTextColor(fg, bg);
    M5Cardputer.Display.setCursor(4, 2);
    M5Cardputer.Display.print("Emulator Options \xbb ");
    M5Cardputer.Display.print(title);
    M5Cardputer.Display.drawFastHLine(0, 12, 240, fg);
}

static void drawMenuFooter(const char* hints) {
    uint16_t bg = getMenuBgColor();
    // Dark grey doesn't look great on white background, so use a slightly dim color based on bg
    uint16_t fg = (bg == BLACK) ? TFT_DARKGREY : 0x7BEF;
    M5Cardputer.Display.setTextColor(fg, bg);
    M5Cardputer.Display.setCursor(4, 135 - 10);
    M5Cardputer.Display.print(hints);
}

static void drawMenuList(int num_items, int selected, const char* items[], int active_idx = -1) {
    uint16_t fg = getMenuColor();
    uint16_t bg = getMenuBgColor();
    const int lineH = 9;
    const int startY = 15;
    const int maxVisible = (135 - startY - 12) / lineH;

    int firstVisible = 0;
    if (selected >= maxVisible) firstVisible = selected - maxVisible + 1;

    for (int i = firstVisible; i < num_items && (i - firstVisible) < maxVisible; i++) {
        int y = startY + (i - firstVisible) * lineH;
        
        bool isActive = (i == active_idx);
        String label = String(i + 1) + ". " + items[i];
        if (isActive) label += " *";
        
        // Truncate if too long (approx 38-39 chars for 240px wide font0)
        if (label.length() > 38) {
            label = label.substring(0, 35) + "...";
        }
        
        if (i == selected) {
            M5Cardputer.Display.fillRect(0, y - 1, 240, lineH, fg);
            M5Cardputer.Display.setTextColor(bg, fg);
        } else {
            M5Cardputer.Display.setTextColor(fg, bg);
        }
        
        M5Cardputer.Display.setCursor(4, y);
        M5Cardputer.Display.print(label);
    }
}

// Submenus
static void menuSelectDisk(int* target_disk_idx, const char* title) {
    String curDir = "";
    int sel = 0;
    bool redraw = true;
    
    // Attempt to initialize sel based on current selection path
    if (*target_disk_idx >= 0 && *target_disk_idx < cntr) {
        String path = Fnames[*target_disk_idx];
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash != -1) {
            curDir = path.substring(0, lastSlash);
        }
    }

    while(true) {
        String visibleNames[70];
        int visibleFIdx[70]; 
        int visibleType[70]; // 0: file, 1: dir, 2: empty, 3: back
        int vCount = 0;

        if (curDir == "") {
            visibleNames[vCount] = "[ Empty ]";
            visibleFIdx[vCount] = -1;
            visibleType[vCount] = 2;
            vCount++;
        } else {
            visibleNames[vCount] = ".. [Back to Parent]";
            visibleFIdx[vCount] = -1;
            visibleType[vCount] = 3;
            vCount++;
        }

        String foundFolders[64];
        int fCount = 0;

        for (int i=0; i<cntr; i++) {
            String path = Fnames[i];
            bool matches = false;
            String rel = "";
            if (curDir == "") {
                matches = true;
                rel = path;
            } else if (path.startsWith(curDir + "/")) {
                matches = true;
                rel = path.substring(curDir.length() + 1);
            }

            if (matches) {
                int slash = rel.indexOf('/');
                if (slash == -1) {
                    visibleNames[vCount] = rel;
                    visibleFIdx[vCount] = i;
                    visibleType[vCount] = 0;
                    vCount++;
                } else {
                    String folderName = rel.substring(0, slash);
                    bool alreadyFound = false;
                    for (int j=0; j<fCount; j++) {
                        if (foundFolders[j] == folderName) { alreadyFound = true; break; }
                    }
                    if (!alreadyFound && fCount < 64) {
                        foundFolders[fCount++] = folderName;
                    }
                }
            }
        }

        for (int i=0; i<fCount; i++) {
            visibleNames[vCount] = "> " + foundFolders[i] + "/";
            visibleFIdx[vCount] = i; 
            visibleType[vCount] = 1;
            vCount++;
        }

        if (sel >= vCount) sel = vCount - 1;
        if (sel < 0) sel = 0;

        if (redraw) {
            String header = String(title);
            if (curDir != "") header += " (/" + curDir + ")";
            drawMenuHeader(header.c_str());
            
            const char* items[70];
            int activeIdx = -1;
            for(int i=0; i<vCount; i++) {
                items[i] = visibleNames[i].c_str();
                if (visibleType[i] == 0 && visibleFIdx[i] == *target_disk_idx) activeIdx = i;
                if (visibleType[i] == 2 && *target_disk_idx == -1) activeIdx = i;
            }
            
            drawMenuList(vCount, sel, items, activeIdx);
            drawMenuFooter("; Up  . Down  Enter Open/Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed()) {
            waitForKeyRelease();
            request_soft_reset = true; // Signal exit to emulator
            return;
        }

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < vCount - 1) { sel++; redraw = true; } }
            }
            if (status.enter) {
                if (visibleType[sel] == 0) { // File
                    *target_disk_idx = visibleFIdx[sel];
                    waitForKeyRelease();
                    return;
                } else if (visibleType[sel] == 1) { // Folder
                    String folderName = visibleNames[sel].substring(2);
                    folderName.remove(folderName.length()-1); // remove trailing slash
                    if (curDir == "") curDir = folderName;
                    else curDir = curDir + "/" + folderName;
                    sel = 0;
                    redraw = true;
                } else if (visibleType[sel] == 2) { // Empty
                    *target_disk_idx = -1;
                    waitForKeyRelease();
                    return;
                } else if (visibleType[sel] == 3) { // Back
                    int lastSlash = curDir.lastIndexOf('/');
                    if (lastSlash == -1) curDir = "";
                    else curDir = curDir.substring(0, lastSlash);
                    sel = 0;
                    redraw = true;
                }
                waitForKeyRelease();
            }
            
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                if (curDir != "") {
                    int lastSlash = curDir.lastIndexOf('/');
                    if (lastSlash == -1) curDir = "";
                    else curDir = curDir.substring(0, lastSlash);
                    sel = 0;
                    redraw = true;
                    waitForKeyRelease();
                } else {
                    waitForKeyRelease();
                    return;
                }
            }
        }
        delay(20);
    }
}

// Submenus removed to simplify

static void menuTerminalColor() {
    int sel = current_options.term_color;
    const char* items[] = {"Green (Phosphor)", "Amber (Phosphor)", "White", "Paper (Dark on Light)"};
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("Text and Terminal Colour");
            drawMenuList(4, sel, items, current_options.term_color);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 3) { sel++; redraw = true; } }
            }
            if (status.enter) {
                current_options.term_color = (TermColor)sel;
                waitForKeyRelease();
                return;
            }
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuBrightness() {
    int sel = 0;
    int bvals[] = {51, 102, 153, 204, 255}; // ~20, 40, 60, 80, 100%
    for(int i=0; i<5; i++) { if (current_options.brightness <= bvals[i]) { sel = i; break; } }
    
    const char* items[] = {"20%", "40%", "60%", "80%", "100%"};
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("Brightness");
            drawMenuList(5, sel, items, sel);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 4) { sel++; redraw = true; } }
            }
            if (status.enter) {
                current_options.brightness = bvals[sel];
                M5Cardputer.Display.setBrightness(bvals[sel]); // apply immediately
                waitForKeyRelease();
                return;
            }
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuFontSize() {
    int sel = current_options.font_size == 1 ? 0 : 1;
    const char* items[] = {"Normal (1x)", "Large (2x)"};
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("Terminal Font Size");
            drawMenuList(2, sel, items, current_options.font_size == 1 ? 0 : 1);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 1) { sel++; redraw = true; } }
            }
            if (status.enter) {
                current_options.font_size = (sel == 0) ? 1 : 2;
                saveOptions();
                extern void apply_canvas_font_size();
                apply_canvas_font_size();
                waitForKeyRelease();
                return;
            }
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuCpuModel() {
    int sel = current_options.cpu_model;
    const char* items[] = {
        "PDP-11/40  (18-bit Unibus)",
        "PDP-11/23  (22-bit Q-Bus, F-11)"
    };
    bool redraw = true;
    while (true) {
        if (redraw) {
            drawMenuHeader("CPU Model");
            drawMenuList(2, sel, items, current_options.cpu_model);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 1) { sel++; redraw = true; } }
            }
            if (status.enter) {
                current_options.cpu_model = (CpuModel)sel;
                waitForKeyRelease();
                return;
            }
            bool esc_pressed = status.del;
            for (auto ch : status.word) { if (ch == 27 || ch == '`') esc_pressed = true; }
            if (esc_pressed) { waitForKeyRelease(); return; }
        }
        delay(20);
    }
}

static void menuBootDevice() {
    int sel = (int)current_options.boot_device;
    bool redraw = true;
    const char* items[] = { "RK05 (Disk 0)", "RL01/02 (Disk 0)" };
    while(true) {
        if (redraw) {
            drawMenuHeader("Select Boot Device");
            drawMenuList(2, sel, items, (int)current_options.boot_device);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) { request_soft_reset = true; return; }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 1) { sel++; redraw = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter) {
                current_options.boot_device = (BootDevice)sel;
                saveOptions();
                waitForKeyRelease();
                return;
            }
            if (esc_pressed) { waitForKeyRelease(); return; }
        }
        delay(20);
    }
}

static void menuDiskLED() {
    int sel = current_options.led_enabled ? 0 : 1;
    bool redraw = true;
    const char* items[] = { "Enabled", "Disabled" };
    while(true) {
        if (redraw) {
            drawMenuHeader("Disk Activity LED");
            drawMenuList(2, sel, items, current_options.led_enabled ? 0 : 1);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) { request_soft_reset = true; return; }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < 1) { sel++; redraw = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter) {
                current_options.led_enabled = (sel == 0);
                saveOptions();
                waitForKeyRelease();
                return;
            }
            if (esc_pressed) { waitForKeyRelease(); return; }
        }
        delay(20);
    }
}

static void menuRKDrives() {
    int sel = 0;
    bool redraw = true;
    while(true) {
        if (redraw) {
            String rk0 = "RK0: " + (current_options.rk_disks[0] >= 0 ? Fnames[current_options.rk_disks[0]] : "[Empty]");
            String rk1 = "RK1: " + (current_options.rk_disks[1] >= 0 ? Fnames[current_options.rk_disks[1]] : "[Empty]");
            String rk2 = "RK2: " + (current_options.rk_disks[2] >= 0 ? Fnames[current_options.rk_disks[2]] : "[Empty]");
            String rk3 = "RK3: " + (current_options.rk_disks[3] >= 0 ? Fnames[current_options.rk_disks[3]] : "[Empty]");
            const char* items[] = { rk0.c_str(), rk1.c_str(), rk2.c_str(), rk3.c_str() };
            drawMenuHeader("RK05 Drive Config");
            drawMenuList(4, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) { request_soft_reset = true; return; }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool handled = false;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; handled = true; } }
                if (ch == '.') { if (sel < 3) { sel++; redraw = true; handled = true; } }
                if (ch == 27 || ch == '`') { waitForKeyRelease(); return; }
            }
            if (status.enter && !handled) {
                char title[32];
                snprintf(title, sizeof(title), "Select RK05 Drive %d", sel);
                menuSelectDisk(&current_options.rk_disks[sel], title);
                redraw = true;
            }
            if (status.del) { waitForKeyRelease(); return; }
        }
        delay(20);
    }
}

static void menuRLDrives() {
    int sel = 0;
    bool redraw = true;
    while(true) {
        if (redraw) {
            String rl0 = "RL0: " + (current_options.rl_disks[0] >= 0 ? Fnames[current_options.rl_disks[0]] : "[Empty]");
            String rl1 = "RL1: " + (current_options.rl_disks[1] >= 0 ? Fnames[current_options.rl_disks[1]] : "[Empty]");
            String rl2 = "RL2: " + (current_options.rl_disks[2] >= 0 ? Fnames[current_options.rl_disks[2]] : "[Empty]");
            String rl3 = "RL3: " + (current_options.rl_disks[3] >= 0 ? Fnames[current_options.rl_disks[3]] : "[Empty]");
            const char* items[] = { rl0.c_str(), rl1.c_str(), rl2.c_str(), rl3.c_str() };
            drawMenuHeader("RL01/02 Drive Config");
            drawMenuList(4, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) { request_soft_reset = true; return; }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool handled = false;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; handled = true; } }
                if (ch == '.') { if (sel < 3) { sel++; redraw = true; handled = true; } }
                if (ch == 27 || ch == '`') { waitForKeyRelease(); return; }
            }
            if (status.enter && !handled) {
                char title[32];
                snprintf(title, sizeof(title), "Select RL01/02 Drive %d", sel);
                menuSelectDisk(&current_options.rl_disks[sel], title);
                redraw = true;
            }
            if (status.del) { waitForKeyRelease(); return; }
        }
        delay(20);
    }
}

static void menuBattery() {
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("Battery Status");
            uint16_t fg = getMenuColor();
            uint16_t bg = getMenuBgColor();
            M5Cardputer.Display.setTextColor(fg, bg);
            
            float vol = M5Cardputer.Power.getBatteryVoltage() / 1000.0;
            
            // Manual calculation since hardware charging status isn't reliable on Cardputer
            int pct = (int)((vol - 3.3) / (4.15 - 3.3) * 100);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            
            M5Cardputer.Display.setCursor(4, 20);
            M5Cardputer.Display.printf("Est. Level: %d %%\n", pct);
            M5Cardputer.Display.setCursor(4, 35);
            M5Cardputer.Display.printf("Voltage:    %.2f V\n", vol);
            M5Cardputer.Display.setCursor(4, 50);
            M5Cardputer.Display.printf("Est. Level based on voltage.");
            
            drawMenuFooter("Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuSystemInfo() {
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("System Info");
            uint16_t fg = getMenuColor();
            uint16_t bg = getMenuBgColor();
            M5Cardputer.Display.setTextColor(fg, bg);
            
            uint32_t free_ram = ESP.getFreeHeap();
            uint32_t min_free_ram = ESP.getMinFreeHeap();
            uint64_t sd_total = SD.totalBytes() / (1024ULL * 1024ULL);
            uint64_t sd_used  = SD.usedBytes()  / (1024ULL * 1024ULL);
            
            M5Cardputer.Display.setCursor(4, 20);
            M5Cardputer.Display.printf("Version:      0.1.3\n");
            M5Cardputer.Display.setCursor(4, 31);
            M5Cardputer.Display.printf("CPU Model:    %s\n",
                current_options.cpu_model == CPU_PDP1123 ? "PDP-11/23" : "PDP-11/40");
            M5Cardputer.Display.setCursor(4, 42);
            M5Cardputer.Display.printf("Free RAM:     %lu KB\n", (unsigned long)(free_ram / 1024));
            M5Cardputer.Display.setCursor(4, 53);
            M5Cardputer.Display.printf("Min Free RAM: %lu KB\n", (unsigned long)(min_free_ram / 1024));
            M5Cardputer.Display.setCursor(4, 64);
            M5Cardputer.Display.printf("SD Total:     %llu MB\n", sd_total);
            M5Cardputer.Display.setCursor(4, 75);
            M5Cardputer.Display.printf("SD Used:      %llu MB\n", sd_used);
            
            drawMenuFooter("Esc Back");
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuEmulationSettings() {
    int sel = 0;
    bool redraw = true;
    int num_items = 4;
    while(true) {
        if (redraw) {
            const char* cpu_name = (current_options.cpu_model == CPU_PDP1123)
                ? "CPU Model: PDP-11/23" : "CPU Model: PDP-11/40";
                
            const char* boot_dev_name = (current_options.boot_device == BOOT_RL) ? "Boot Device: RL01/02" : "Boot Device: RK05";

            const char* items[] = {
                cpu_name,
                boot_dev_name,
                "RK05 Drives Config",
                "RL01/02 Drives Config"
            };
            drawMenuHeader("Emulation Settings");
            drawMenuList(4, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true; 
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool handled = false;
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; handled = true; } }
                if (ch == '.') { if (sel < num_items - 1) { sel++; redraw = true; handled = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter && !handled) {
                switch(sel) {
                    case 0: menuCpuModel(); break;
                    case 1: menuBootDevice(); break;
                    case 2: menuRKDrives(); break;
                    case 3: menuRLDrives(); break;
                }
                redraw = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

static void menuCardputerSettings() {
    int sel = 0;
    bool redraw = true;
    int num_items = 4;
    while(true) {
        if (redraw) {
            const char* items[] = {
                "Text Colour",
                "Brightness",
                "Terminal Font Size",
                "Disk Activity LED",
                "Battery Status"
            };
            drawMenuHeader("Cardputer Settings");
            drawMenuList(5, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed() || request_soft_reset) {
            request_soft_reset = true;
            return;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool handled = false;
            bool esc_pressed = status.del;
            int num_items = 5;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; handled = true; } }
                if (ch == '.') { if (sel < num_items - 1) { sel++; redraw = true; handled = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter && !handled) {
                switch(sel) {
                    case 0: menuTerminalColor(); break;
                    case 1: menuBrightness(); break;
                    case 2: menuFontSize(); break;
                    case 3: menuDiskLED(); break;
                    case 4: menuBattery(); break;
                }
                redraw = true;
            }
            if (esc_pressed) {
                waitForKeyRelease();
                return;
            }
        }
        delay(20);
    }
}

void createSnapshot() {
    SD.mkdir("/snapshots");
    
    // Find next snapshot number
    int snap_idx = 0;
    String snap_dir;
    while (true) {
        snap_dir = String("/snapshots/snap_") + snap_idx;
        if (!SD.exists(snap_dir.c_str())) {
            break;
        }
        snap_idx++;
    }
    SD.mkdir(snap_dir.c_str());

    // Save current options
    File f_cfg = SD.open((snap_dir + "/config.bin").c_str(), FILE_WRITE);
    if (f_cfg) {
        f_cfg.write((uint8_t*)&current_options, sizeof(current_options));
        f_cfg.close();
    }

    // Save CPU state
    extern KB11 cpu;
    cpu.saveSnapshot(snap_dir.c_str());

    // Show message
    M5Cardputer.Display.fillRect(0, 135 - 20, 240, 20, TFT_DARKGREEN);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    M5Cardputer.Display.setCursor(4, 135 - 16);
    M5Cardputer.Display.printf("Snapshot saved: snap_%d", snap_idx);
    M5Cardputer.update();
    delay(1500);
}

void deleteRecursive(const char* path) {
    File root = SD.open(path);
    if (!root) return;
    if (!root.isDirectory()) {
        root.close();
        SD.remove(path);
        return;
    }
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        // On ESP32 SD library, entry.name() might be just the basename or full path.
        // Assuming basename.
        String entryName = entry.name();
        String entryPath = String(path);
        if (!entryPath.endsWith("/")) entryPath += "/";
        entryPath += entryName;
        
        if (entry.isDirectory()) {
            entry.close();
            deleteRecursive(entryPath.c_str());
        } else {
            entry.close();
            SD.remove(entryPath.c_str());
        }
    }
    root.close();
    SD.rmdir(path);
}

String textInput(const char* title, const char* initial) {
    String buffer = initial;
    bool redraw = true;
    while(true) {
        if (redraw) {
            M5Cardputer.Display.fillRect(0, 30, 240, 80, TFT_BLACK);
            M5Cardputer.Display.drawRect(5, 50, 230, 40, getMenuColor());
            M5Cardputer.Display.setCursor(10, 40);
            M5Cardputer.Display.setTextColor(TFT_YELLOW);
            M5Cardputer.Display.print(title);
            M5Cardputer.Display.setCursor(10, 65);
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.setTextSize(2);
            M5Cardputer.Display.print(buffer);
            M5Cardputer.Display.print("_");
            M5Cardputer.Display.setTextSize(1);
            redraw = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            if (status.del && buffer.length() > 0) {
                buffer.remove(buffer.length() - 1);
                redraw = true;
            }
            if (status.enter) {
                waitForKeyRelease();
                return buffer;
            }
            for (auto ch : status.word) {
                if (ch == 27 || ch == '`') return ""; // Cancel
                if (ch >= 32 && ch <= 126 && buffer.length() < 24) {
                    buffer += ch;
                    redraw = true;
                }
            }
        }
        delay(20);
    }
}

void menuManageSnapshots() {
    while(true) {
        File root = SD.open("/snapshots");
        if (!root) {
            SD.mkdir("/snapshots");
            root = SD.open("/snapshots");
        }
        
        String snapshots[40];
        int count = 0;
        while (true) {
            File entry = root.openNextFile();
            if (!entry) break;
            if (entry.isDirectory()) {
                if (count < 40) snapshots[count++] = entry.name();
            }
            entry.close();
        }
        root.close();

        if (count == 0) {
            drawMenuHeader("Manage Snapshots");
            M5Cardputer.Display.setCursor(10, 60);
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.print("No snapshots found.");
            M5Cardputer.update();
            delay(1000);
            return;
        }

        int sel = 0;
        bool redraw = true;
        while(true) {
            if (redraw) {
                drawMenuHeader("Manage Snapshots");
                const char* items[40];
                for (int i=0; i<count; i++) items[i] = snapshots[i].c_str();
                drawMenuList(count, sel, items);
                drawMenuFooter("Enter: Options  Esc: Back");
                redraw = false;
            }

            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto status = M5Cardputer.Keyboard.keysState();
                bool esc_pressed = status.del;
                for (auto ch : status.word) {
                    if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                    if (ch == '.') { if (sel < count - 1) { sel++; redraw = true; } }
                    if (ch == 27 || ch == '`') esc_pressed = true;
                }
                if (status.enter) {
                    waitForKeyRelease();
                    // Sub-menu for selection
                    int sub_sel = 0;
                    bool sub_redraw = true;
                    const char* sub_items[] = {"Rename", "Delete", "Cancel"};
                    while(true) {
                        if (sub_redraw) {
                            M5Cardputer.Display.fillRect(60, 40, 120, 70, TFT_BLACK);
                            M5Cardputer.Display.drawRect(60, 40, 120, 70, getMenuColor());
                            for(int i=0; i<3; i++) {
                                M5Cardputer.Display.setCursor(70, 50 + i*20);
                                if (i == sub_sel) M5Cardputer.Display.setTextColor(TFT_YELLOW);
                                else M5Cardputer.Display.setTextColor(TFT_WHITE);
                                M5Cardputer.Display.print(sub_items[i]);
                            }
                            M5Cardputer.update();
                            sub_redraw = false;
                        }
                        M5Cardputer.update();
                        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                            auto s_status = M5Cardputer.Keyboard.keysState();
                            for (auto ch : s_status.word) {
                                if (ch == ';') { if (sub_sel > 0) { sub_sel--; sub_redraw = true; } }
                                if (ch == '.') { if (sub_sel < 2) { sub_sel++; sub_redraw = true; } }
                            }
                            if (s_status.enter) {
                                if (sub_sel == 0) { // Rename
                                    String oldName = snapshots[sel];
                                    String newName = textInput("New name:", oldName.c_str());
                                    if (newName != "" && newName != oldName) {
                                        SD.rename(("/snapshots/" + oldName).c_str(), ("/snapshots/" + newName).c_str());
                                    }
                                    break; // Refresh list
                                } else if (sub_sel == 1) { // Delete
                                    deleteRecursive(("/snapshots/" + snapshots[sel]).c_str());
                                    break; // Refresh list
                                } else {
                                    break;
                                }
                            }
                            if (s_status.del) break;
                        }
                        delay(20);
                    }
                    redraw = true;
                    break; // Refresh list
                }
                if (esc_pressed) return;
            }
            delay(20);
        }
    }
}

void loadSnapshotMenu() {
    File root = SD.open("/snapshots");
    if (!root) {
        return;
    }
    
    String snapshots[30];
    int count = 0;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (entry.isDirectory() && String(entry.name()).startsWith("snap_")) {
            if (count < 30) snapshots[count++] = entry.name();
        }
        entry.close();
    }
    root.close();

    if (count == 0) return;

    int sel = 0;
    bool redraw = true;
    while(true) {
        if (redraw) {
            drawMenuHeader("Load Snapshot");
            const char* items[30];
            for (int i=0; i<count; i++) items[i] = snapshots[i].c_str();
            drawMenuList(count, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Back");
            redraw = false;
        }

        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; } }
                if (ch == '.') { if (sel < count - 1) { sel++; redraw = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter) {
                snapshot_to_load = String("/snapshots/") + snapshots[sel];
                request_load_snapshot = true;
                return;
            }
            if (esc_pressed) return;
        }
        delay(20);
    }
}

// Main Options Menu
void openOptionsMenu() {
    int sel = 0;
    EmulatorOptions backup = current_options;
    
    bool redraw = true;
    int num_items = 7;
    while(true) {
        if (redraw) {
            const char* items[] = {
                "Emulation Settings",
                "Cardputer Settings",
                "System Info",
                "Create Snapshot",
                "Load Snapshot",
                "Manage Snapshots",
                "System Reset"
            };
            
            drawMenuHeader("Main Menu");
            drawMenuList(num_items, sel, items);
            drawMenuFooter("; Up  . Down  Enter Select  Esc Exit / G0 Save");
            redraw = false;
        }
        
        M5Cardputer.update();
        bool g0_pressed = M5Cardputer.BtnA.wasPressed();
        
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto status = M5Cardputer.Keyboard.keysState();
            bool handled = false;
            bool esc_pressed = status.del;
            for (auto ch : status.word) {
                if (ch == ';') { if (sel > 0) { sel--; redraw = true; handled = true; } }
                if (ch == '.') { if (sel < num_items - 1) { sel++; redraw = true; handled = true; } }
                if (ch == 27 || ch == '`') esc_pressed = true;
            }
            if (status.enter && !handled) {
                switch(sel) {
                    case 0: menuEmulationSettings(); break;
                    case 1: menuCardputerSettings(); break;
                    case 2: menuSystemInfo(); break;
                    case 3: createSnapshot(); break;
                    case 4: loadSnapshotMenu(); break;
                    case 5: menuManageSnapshots(); break;
                    case 6:
                        request_soft_reset = true;
                        waitForKeyRelease();
                        return;
                }
                redraw = true;
            }
            if (esc_pressed || g0_pressed || request_soft_reset || request_load_snapshot) {
                bool signal_exit = request_soft_reset || request_load_snapshot;
                request_soft_reset = false;
                saveOptions();
                applyOptions();
                bool disk_changed = false;
                for(int i=0; i<4; i++) {
                    if (backup.rk_disks[i] != current_options.rk_disks[i]) disk_changed = true;
                    if (backup.rl_disks[i] != current_options.rl_disks[i]) disk_changed = true;
                }
                if (disk_changed || backup.cpu_model != current_options.cpu_model || signal_exit) {
                    request_soft_reset = true;
                }
                waitForKeyRelease();
                return;
            }
        } else if (g0_pressed) {
            saveOptions();
            applyOptions();
            bool disk_changed = false;
            for(int i=0; i<4; i++) {
                if (backup.rk_disks[i] != current_options.rk_disks[i]) disk_changed = true;
                if (backup.rl_disks[i] != current_options.rl_disks[i]) disk_changed = true;
            }
            if (disk_changed || backup.cpu_model != current_options.cpu_model) {
                request_soft_reset = true;
            }
            return;
        }
        delay(20);
    }
}


