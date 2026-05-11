#pragma once
#include <stdint.h>
#include <FS.h>
#include <SD.h>


class KW11 {
  public:
    void write16(uint32_t a, uint16_t v);
    uint16_t read16(uint32_t a);

    KW11();
    void tick();
    void saveSnapshot(File f);
    void loadSnapshot(File f);

  private:
    uint16_t csr;
};