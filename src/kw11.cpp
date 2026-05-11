#include "kw11.h"
#include "avr11.h"
#include "kb11.h"
#include <iostream>
#include <signal.h>
#include <Arduino.h>
#include <SD.h>

extern KB11 cpu;

void kw11alarm(int) { cpu.unibus.kw11.tick(); }

KW11::KW11() {
}

void KW11::write16(uint32_t a, uint16_t v) {
    switch (a) {
    case 0777546:
        // Serial.printf("kw11: csr write %06o\n", v);
        csr = v;
        return;
    default:
        Serial.printf("kw11: write to invalid address %06o\n", a);
        trap(INTBUS);
    }
}

uint16_t KW11::read16(uint32_t a) {
    uint16_t cxsr;
    switch (a) {
    case 0777546:
        // Serial.printf("kw11: csr read %06o\n", csr);
        cxsr = csr;
        csr &= ~0200;
        return cxsr;
    default:
        Serial.printf("kw11: read from invalid address %06o\n", a);
        trap(INTBUS);
    }
}

void KW11::tick() {
    csr |= (1 << 7);
    if (csr & (1 << 6)) {
        cpu.interrupt(INTCLOCK, 6);
    }
}

void KW11::saveSnapshot(File f) { f.write((uint8_t*)&csr, sizeof(csr)); }
void KW11::loadSnapshot(File f) { f.read((uint8_t*)&csr, sizeof(csr)); }
