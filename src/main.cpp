// PDP-11/40 + 11/24 emulator — ported to M5Stack Cardputer
// Original: Isysxp/PDP11-on-the-M5-Core (M5Core2, touch screen)
// Port:     M5Stack Cardputer (ESP32-S3, 240×135 ST7789V2, physical 56-key keyboard)
//
// HARDWARE RESET: The physical side button on the Cardputer performs a hard reset
// of the ESP32-S3 at silicon level — no software code needed.
//
// SD card CS pin: GPIO 12 (handled internally by M5Cardputer BSP).
// WiFi credentials: /Wifi.txt on SD card (line1 = SSID, line2 = password).

#include "M5Cardputer.h"
#include <Arduino.h>
#include <SD.h>
#include <FS.h>
#include <ESP32Time.h>
#include <FastLED.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "options.h"
#include "kb11.h"

// ── Forward declarations ──────────────────────────────────────────────────────
int startup(int bootdev);

// ── Canvas for PDP-11 terminal output ────────────────────────────────────────
M5Canvas canvas(&M5Cardputer.Display);

using namespace std;

// ── Shared state ─────────────────────────────────────────────────────────────
String Fnames[64];
int    cntr    = 0;   // total .RK05/.RL02 images found on SD
int    SelFile = 0;   // currently highlighted menu entry
extern int runFlag;
String ssid, pswd;

static uint32_t lastPush = 0;
bool display_dirty = false;

void update_canvas_colors() {
    uint16_t fg = TFT_GREEN;
    uint16_t bg = BLACK;
    if (current_options.term_color == COLOR_AMBER) fg = 0xFFB000;
    else if (current_options.term_color == COLOR_WHITE) fg = TFT_WHITE;
    else if (current_options.term_color == COLOR_PAPER) { fg = BLACK; bg = 0xFFFFCC; }
    
    canvas.setPaletteColor(0, bg);
    canvas.setPaletteColor(1, fg);
    canvas.setTextColor(1, 0);
}

void console_output_char(char c) {
    if (c == 0) return;
    
    if (c == '\a' || c == 0x07) {
        M5Cardputer.Speaker.tone(880, 150);
        return;
    }
    
    canvas.print(c);
    display_dirty = true;
}

void flush_console() {
    if (display_dirty && (millis() - lastPush >= 40)) { // Max 25 FPS
        canvas.pushSprite(0, 0);
        lastPush = millis();
        display_dirty = false;
    }
}

CRGB leds[1];

void setDiskLED(bool write, bool active) {
    if (!current_options.led_enabled) return;
    if (!active) {
        leds[0] = CRGB::Black;
    } else {
        leds[0] = write ? CRGB::Red : CRGB::Blue;
    }
    FastLED.show();
}

void tickDiskLED() {
    // No longer needed
}

// ── Soft Reset ────────────────────────────────────────────────────────────────
extern KB11 cpu; // from kb11.h

void perform_soft_reset() {
    // Close old files
    for(int i=0; i<4; i++) {
        if (cpu.unibus.rk11.rk05[i]) { cpu.unibus.rk11.rk05[i].close(); cpu.unibus.rk11.rk05[i] = File(); }
        if (cpu.unibus.rl11.rl02[i]) { cpu.unibus.rl11.rl02[i].close(); cpu.unibus.rl11.rl02[i] = File(); }
    }
    
    // Open new files
    int bootdev = 0;
    for(int i=0; i<4; i++) {
        if (current_options.rk_disks[i] >= 0) {
            String path = "/pdp11/" + Fnames[current_options.rk_disks[i]];
            cpu.unibus.rk11.rk05[i] = SD.open(path.c_str(), "rb+");
        }
        if (current_options.rl_disks[i] >= 0) {
            String path = "/pdp11/" + Fnames[current_options.rl_disks[i]];
            cpu.unibus.rl11.rl02[i] = SD.open(path.c_str(), "rb+");
        }
    }
    if (current_options.rk_disks[0] < 0 && current_options.rl_disks[0] >= 0) bootdev = 1;
    
    if (current_options.rl_disks[0] >= 0 && strcasestr(Fnames[current_options.rl_disks[0]].c_str(), ".rl02")) {
        extern int RLTYPE;
        RLTYPE = 0235;
    } else {
        extern int RLTYPE;
        RLTYPE = 035;
    }
    
    cpu.reset(02002, bootdev);
    
    canvas.fillSprite(0);
    canvas.setCursor(0, 0);
    canvas.printf("[SYSTEM RESET]\r\n");
    canvas.pushSprite(0, 0);
    delay(500); 
}

void redraw_terminal() {
    // No-op - we removed the buffer
}

// ── Keyboard injection into KL11 ─────────────────────────────────────────────
// keypressed is declared here; kl11.cpp externs it.
// keypressed is owned by kl11.cpp (where it is used); declared extern here
extern bool keypressed;
static char kbuf = 0;

void kl11_rx_char(char c) {
    kbuf      = c;
    keypressed = true;
}
char kl11_get_kbuf() {
    char c = kbuf;
    kbuf   = 0;
    return c;
}

// ── SD scan for disk images ───────────────────────────────────────────────────
static void scanDiskImages(fs::FS &fs, const char *dirname, const char *basedir) {
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;
    File file = root.openNextFile();
    while (file && cntr < 64) {
        if (file.isDirectory()) {
            scanDiskImages(fs, file.path(), basedir);
        } else {
            const char *path = file.path();
            // Store path relative to basedir (e.g. "/pdp11/pdp11-40/foo.rk05" → "pdp11-40/foo.rk05")
            const char *rel = path;
            size_t baselen = strlen(basedir);
            if (strncmp(path, basedir, baselen) == 0) {
                rel = path + baselen;
                if (*rel == '/') rel++;
            }
            if (strcasestr(rel, ".rk05") || strcasestr(rel, ".rl0")) {
                Fnames[cntr++] = rel;
                Serial.printf("  [%d] %s  (%lu B)\r\n", cntr, rel,
                              (unsigned long)file.size());
            }
        }
        file = root.openNextFile();
    }
}

// ── setup() ──────────────────────────────────────────────────────────────────
void setup() {
    // Delay to allow power rails to stabilize (mitigates Cardputer brownout/boot loop)
    delay(250);

    char rkfile[64] = {0};
    char rlfile[64] = {0};
    int  bootdev    = 0;

    // Hardware init
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);          // true = enable keyboard
    
    FastLED.addLeds<WS2812, 21, GRB>(leds, 1);
    FastLED.setBrightness(32);
    leds[0] = CRGB::Black;
    FastLED.show();
    
    M5Cardputer.Display.setRotation(1);    // landscape 240×135
    
    // Load options from NVS
    loadOptions();
    
    M5Cardputer.Display.setBrightness(current_options.brightness);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);
    delay(500);
    Serial.println("Cardputer PDP-11 starting...");

    // Canvas for the emulator terminal (1-bit color depth to save ~60KB RAM)
    canvas.setColorDepth(1);
    canvas.createSprite(240, 135);
    canvas.setTextScroll(true);
    canvas.setFont(&fonts::Font0);  // 6×8 mono → ~39 col × 16 rows
    update_canvas_colors();
    canvas.fillSprite(0);
    canvas.pushSprite(0, 0);
    
    M5Cardputer.Speaker.setVolume(128);

    // Mount SD card
    // Cardputer SD SPI pins: SCK=40, MISO=39, MOSI=14, CS=12
    // These are non-default, so SPI must be explicitly initialized first.
    SPI.begin(40, 39, 14, 12);  // SCK, MISO, MOSI, CS
    if (!SD.begin(12, SPI, 25000000)) {
        Serial.println("SD Card mount failed!");
        M5Cardputer.Display.setTextColor(TFT_RED, BLACK);
        M5Cardputer.Display.setCursor(4, 4);
        M5Cardputer.Display.print("SD Card mount failed!");
        M5Cardputer.Display.setCursor(4, 14);
        M5Cardputer.Display.print("Check: FAT32, card seated");
        while (1) delay(1000);
    }
    Serial.printf("SD: %llu MB  Free heap: %d  Free PSRAM: %lu\r\n",
                  SD.totalBytes() / (1024ULL * 1024ULL),
                  ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram());

    // Scan SD for disk images in /pdp11/ (recursive)
    scanDiskImages(SD, "/pdp11", "/pdp11");
    if (cntr == 0) {
        Serial.println("No .RK05/.RL02 images in /pdp11/!");
        M5Cardputer.Display.setTextColor(TFT_RED, BLACK);
        M5Cardputer.Display.setCursor(4, 4);
        M5Cardputer.Display.print("No disk images in /pdp11/!");
        M5Cardputer.Display.setCursor(4, 14);
        M5Cardputer.Display.print("Put .rk05/.rl0x in /pdp11/");
        while (1) delay(1000);
    }

    // Show Options Menu before booting
    openOptionsMenu();
    
    // Check if soft reset requested a specific disk from the menu
    if (request_soft_reset) {
        request_soft_reset = false;
    }

    // Open new files based on current_options
    bootdev = 0;
    for(int i=0; i<4; i++) {
        if (current_options.rk_disks[i] >= 0) {
            String path = "/pdp11/" + Fnames[current_options.rk_disks[i]];
            cpu.unibus.rk11.rk05[i] = SD.open(path.c_str(), "rb+");
        }
        if (current_options.rl_disks[i] >= 0) {
            String path = "/pdp11/" + Fnames[current_options.rl_disks[i]];
            cpu.unibus.rl11.rl02[i] = SD.open(path.c_str(), "rb+");
        }
    }
    bootdev = (int)current_options.boot_device;

    Serial.printf("bootdev=%d\r\n", bootdev);

    // Switch display to terminal
    canvas.fillSprite(0);
    canvas.setCursor(0, 0);
    canvas.printf("Booting...\r\n");
    canvas.pushSprite(0, 0);

    // Hand off to emulator — never returns
    startup(bootdev);
}

// loop() is never reached — startup() contains its own infinite loop.
// M5Cardputer.update() is called inside avr11.cpp's poll cycle.
void loop() {}
