// Host stub of Arduino Wire (TwoWire) backed by a simulated Si5351 register file.
#ifndef WIRE_STUB_H
#define WIRE_STUB_H
#include <stdint.h>
#include <stddef.h>

class TwoWire {
public:
  void begin();
  void beginTransmission(uint8_t addr);
  size_t write(uint8_t b);
  uint8_t endTransmission(void);
  uint8_t endTransmission(uint8_t sendStop);
  uint8_t requestFrom(uint8_t addr, uint8_t quantity);
  int available(void);
  int read(void);
};
extern TwoWire Wire;
#endif
