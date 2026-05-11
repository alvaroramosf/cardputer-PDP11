#ifndef OPTIONS_H
#define OPTIONS_H

#include <Arduino.h>

enum TermColor {
    COLOR_GREEN = 0,
    COLOR_AMBER = 1,
    COLOR_WHITE = 2,
    COLOR_PAPER = 3
};

enum CpuModel {
    CPU_PDP1140 = 0,  // PDP-11/40: 18-bit Unibus, no MFPT
    CPU_PDP1123 = 1   // PDP-11/23: 22-bit Q-Bus (F-11), MFPT returns 1
};

enum BootDevice {
    BOOT_RK = 0,
    BOOT_RL = 1
};

struct EmulatorOptions {
    int rk_disks[4];  // indices into Fnames, -1 means empty
    int rl_disks[4];  // indices into Fnames, -1 means empty
    TermColor term_color;
    int brightness;
    CpuModel cpu_model;
    BootDevice boot_device;
    bool led_enabled;
    int font_size;
};

extern EmulatorOptions current_options;

void loadOptions();
void saveOptions();
void applyOptions();
void openOptionsMenu();

// Export total disk count and names from main.cpp
extern int cntr;
extern String Fnames[64];

// Notify main that soft reset is requested
extern bool request_soft_reset;
extern bool request_soft_reset;

#endif // OPTIONS_H
