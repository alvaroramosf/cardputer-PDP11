#define _CRT_SECURE_NO_WARNINGS
#include <string>
#include <assert.h>
#include <cstdlib>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avr11.h"
#include "kb11.h"
#include "M5Cardputer.h"
#include <ESP32Time.h>
#include <vector>
#include "options.h"

// ── Keyboard injection API (defined in main.cpp) ──────────────────────────────
extern void kl11_rx_char(char c);
extern char kl11_get_kbuf();
extern void flush_console();

KB11 cpu;
int kbdelay = 0;
uint64_t systime, nowtime, clkdiv;
ESP32Time SystemTime;
int runFlag = 0;
int RLTYPE;

// ── Physical keyboard polling ─────────────────────────────────────────────────
// Called every ~1000 CPU steps inside emulator_loop0().
// Ctrl+letter is converted to ASCII control code (e.g. Ctrl+C → 0x03).
// In the emulation phase ; and . are normal characters (not menu navigation).

// History and editing buffer removed to simplify terminal usage.

static void poll_keyboard() {
    M5Cardputer.update();
    extern void tickFanSound();
    tickFanSound();
    
    if (M5Cardputer.BtnA.wasPressed()) {
        cpu.wtstate = true;  // Pause CPU
        openOptionsMenu();   // Open settings menu directly
        cpu.wtstate = false; // Resume CPU on return
        extern bool display_dirty;
        display_dirty = true;
        return;
    }
    
    while (Serial.available()) {
        kl11_rx_char(Serial.read());
    }
    
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        // Direct Send Logic (Historical VT100 behavior)
        for (auto c : status.word) {
            if (status.ctrl) {
                char cc = c;
                if (cc >= 'a' && cc <= 'z') cc -= 0x60;
                else if (cc >= 'A' && cc <= 'Z') cc -= 0x40;
                kl11_rx_char(cc);
            } else {
                kl11_rx_char(c);
            }
        }
        if (status.del)   kl11_rx_char(0x7F);
        if (status.enter) kl11_rx_char('\r');
        if (status.tab)   kl11_rx_char('\t');
    }
}

// ── PDP-11 setup ──────────────────────────────────────────────────────────────
// Named setup_pdp11 to avoid collision with Arduino's setup() in main.cpp.
static void setup_pdp11(int bootdev) {
    RLTYPE = 035;
    if (current_options.rl_disks[0] >= 0 && strcasestr(Fnames[current_options.rl_disks[0]].c_str(), ".rl02"))
        RLTYPE = 0235;
    clkdiv  = (uint64_t)1000000 / (uint64_t)60;
    systime = millis();
    cpu.reset(02002, bootdev);
    Serial.printf("PDP-11 CPU reset OK, starting emulation...\r\n");
}

jmp_buf trapbuf;

// Forward declaration of the inner loop
void emulator_loop0();

[[noreturn]] void trap(uint16_t vec) { longjmp(trapbuf, vec); }

// emulator_loop() — replaces the Arduino loop() name to avoid ODR conflict.
// Called in a tight while(1) from startup().
void emulator_loop() {
    auto vec = setjmp(trapbuf);
    if (vec == 0) {
        emulator_loop0();
    } else {
        cpu.trapat(vec);
    }
}

void emulator_loop0() {
    while (true) {
        if (request_load_snapshot) {
            extern void perform_load_snapshot(String dir);
            perform_load_snapshot(snapshot_to_load);
            request_load_snapshot = false;
            request_soft_reset = false;
        }

        if (request_soft_reset) {
            extern void perform_soft_reset();
            perform_soft_reset();
            request_soft_reset = false;
        }

        if ((cpu.itab[0].vec > 0) && (cpu.itab[0].pri > cpu.priority())) {
            cpu.trapat(cpu.itab[0].vec);
            cpu.popirq();
            return; // exit to reset trapbuf via setjmp in emulator_loop()
        }
        if (!cpu.wtstate)
            cpu.step();
        cpu.unibus.rk11.step();
        cpu.unibus.rl11.step();

        if (kbdelay++ >= 1000) {
            kbdelay = 0;
            
            // The emulator's virtual console must be polled frequently to trigger
            // TTY interrupts, otherwise the PDP-11 CPU stalls waiting for I/O!
            cpu.unibus.cons.poll();
            cpu.unibus.dl11.poll();
            flush_console(); // internally throttles to 25 FPS

            nowtime = millis();
            // Only poll physical keyboard every 20ms (50 FPS)
            // M5Cardputer.update() is very slow and will choke the CPU if called too often.
            if (nowtime - systime >= 20) {
                poll_keyboard();
                cpu.unibus.kw11.tick();
                systime = nowtime;
            }
        }
    }
}

// startup() is called from main.cpp's setup() and never returns.
int startup(int bootdev) {
    setup_pdp11(bootdev);
    runFlag++;
    while (1)
        emulator_loop();
}

void panic() {
    cpu.printstate();
    std::abort();
}
