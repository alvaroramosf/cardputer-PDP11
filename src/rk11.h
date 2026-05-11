#pragma once
#include <stdint.h>
#include <stdio.h>
#include "FS.h"
#include "SD.h"

class RK11 {

  public:
	  File rk05[4];
    uint16_t read16(uint32_t a);
    void write16(uint32_t a, uint16_t v);
    void reset();
    void step();
    void saveSnapshot(File f);
    void loadSnapshot(File f);

  private:
	uint16_t rkds, rker, rkcs, rkwc, rkba, rkda;
	uint8_t bcnt;
    uint32_t drive, sector, surface, cylinder,rkba18,rkdelay;

    void rknotready();
    void rkready();
    void readwrite();
    void seek();
};
