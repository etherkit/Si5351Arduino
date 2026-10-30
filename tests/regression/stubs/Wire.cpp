#include "Wire.h"
#include "sim.h"
#include <stdio.h>
#include <string.h>

TwoWire Wire;
Si5351Sim g_sim;

static std::string hexbytes(const uint8_t *p, size_t n) {
  std::string s = "[";
  char b[8];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof b, i ? " %02X" : "%02X", p[i]);
    s += b;
  }
  return s + "]";
}

void Si5351Sim::reset(int pattern, bool present_) {
  // pattern 0: all-zero register file; pattern 1: deterministic non-zero
  // fill so read-modify-write bugs (lost/extra bits) become visible.
  for (int i = 0; i < 256; i++)
    regs[i] = pattern ? (uint8_t)((i * 37 + 11) ^ 0xA5) : 0;
  regs[0] &= 0x7F;  // SYS_INIT clear => device ready (init() polls bit 7)
  ptr = 0; dev_addr = 0x60; present = present_;
  in_tx = false; tx_addr = 0; tx_buf.clear();
  rx_buf.clear(); rx_pos = 0;
  events.clear(); n_writes = n_reads = 0;
}

void TwoWire::begin() { g_sim.events.push_back("BEGIN"); }

void TwoWire::beginTransmission(uint8_t addr) {
  g_sim.in_tx = true; g_sim.tx_addr = addr; g_sim.tx_buf.clear();
}

size_t TwoWire::write(uint8_t b) {
  if (!g_sim.in_tx) { g_sim.events.push_back("ERR write-outside-tx"); return 0; }
  g_sim.tx_buf.push_back(b);
  return 1;
}

uint8_t TwoWire::endTransmission(uint8_t) { return endTransmission(); }

uint8_t TwoWire::endTransmission(void) {
  char line[64];
  Si5351Sim &s = g_sim;
  s.in_tx = false;
  bool ack = s.present && s.tx_addr == s.dev_addr;
  if (s.tx_buf.empty()) {
    snprintf(line, sizeof line, "PROBE a=%02X ack=%d", s.tx_addr, ack);
    s.events.push_back(line);
  } else if (s.tx_buf.size() == 1) {
    snprintf(line, sizeof line, "PTR a=%02X r=%u", s.tx_addr, s.tx_buf[0]);
    s.events.push_back(line);
    if (ack) s.ptr = s.tx_buf[0];
  } else {
    uint8_t reg = s.tx_buf[0];
    snprintf(line, sizeof line, "W a=%02X r=%u n=%u ", s.tx_addr, reg,
             (unsigned)(s.tx_buf.size() - 1));
    s.events.push_back(line + hexbytes(&s.tx_buf[1], s.tx_buf.size() - 1));
    if (ack) {
      for (size_t i = 1; i < s.tx_buf.size(); i++) s.regs[(uint8_t)(reg + i - 1)] = s.tx_buf[i];
      s.ptr = (uint8_t)(reg + s.tx_buf.size() - 1);
      s.n_writes++;
    }
  }
  s.tx_buf.clear();
  return ack ? 0 : 2;
}

uint8_t TwoWire::requestFrom(uint8_t addr, uint8_t quantity) {
  Si5351Sim &s = g_sim;
  s.rx_buf.clear(); s.rx_pos = 0;
  bool ack = s.present && addr == s.dev_addr;
  if (ack) {
    for (uint8_t i = 0; i < quantity; i++) s.rx_buf.push_back(s.regs[(uint8_t)(s.ptr + i)]);
    s.ptr = (uint8_t)(s.ptr + quantity);
    s.n_reads++;
  }
  char line[48];
  snprintf(line, sizeof line, "R a=%02X n=%u -> ", addr, quantity);
  s.events.push_back(line + hexbytes(s.rx_buf.data(), s.rx_buf.size()));
  return (uint8_t)s.rx_buf.size();
}

int TwoWire::available(void) { return (int)(g_sim.rx_buf.size() - g_sim.rx_pos); }

int TwoWire::read(void) {
  if (g_sim.rx_pos >= g_sim.rx_buf.size()) return -1;
  return g_sim.rx_buf[g_sim.rx_pos++];
}
