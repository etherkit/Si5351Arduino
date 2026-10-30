// Multi-bus TwoWire stub: each instance has its own register file + transcript.
#ifndef WIRE_STUB_H
#define WIRE_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
class TwoWire {
public:
  TwoWire(const char *n) : name(n) { for (int i=0;i<256;i++) regs[i]=(uint8_t)i; regs[0]=0; }
  void begin() { log.push_back("BEGIN"); }
  void beginTransmission(uint8_t a) { tx_addr=a; tx.clear(); }
  size_t write(uint8_t b) { tx.push_back(b); return 1; }
  uint8_t endTransmission(uint8_t) { return endTransmission(); }
  uint8_t endTransmission(void) {
    bool ack = present && tx_addr==0x60;
    char l[64]; snprintf(l,sizeof l,"TX a=%02X n=%u ack=%d",tx_addr,(unsigned)tx.size(),ack); log.push_back(l);
    if (ack && !tx.empty()) { ptr=tx[0]; for (size_t i=1;i<tx.size();i++) regs[(uint8_t)(ptr+i-1)]=tx[i]; }
    return ack?0:2; }
  uint8_t requestFrom(uint8_t a, uint8_t q) {
    rx.clear(); rp=0; char l[48]; snprintf(l,sizeof l,"RX a=%02X n=%u",a,q); log.push_back(l);
    if (present && a==0x60) for (int i=0;i<q;i++) rx.push_back(regs[(uint8_t)(ptr+i)]);
    return (uint8_t)rx.size(); }
  int available() { return (int)(rx.size()-rp); }
  int read() { return rp<rx.size()? rx[rp++] : -1; }
  const char *name; bool present=false; std::vector<std::string> log;
  uint8_t regs[256], ptr=0, tx_addr=0; std::vector<uint8_t> tx, rx; size_t rp=0;
};
extern TwoWire Wire; extern TwoWire Wire1;
#endif
