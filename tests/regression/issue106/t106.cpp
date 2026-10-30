#include <stdio.h>
#include <string.h>
#include "si5351.h"
TwoWire Wire("Wire"); TwoWire Wire1("Wire1");
static int fails=0;
#define CHECK(c,msg) do{ bool _c=(c); printf("%s  %s\n", _c?"PASS":"FAIL", msg); if(!_c) fails++; }while(0)
static void clear(){ Wire.log.clear(); Wire1.log.clear(); Wire.present=Wire1.present=false; }
int main(){
  // T1 constructor matrix (compiles => overload resolution OK); check selected bus/addr
  { Si5351 a;            CHECK(a._wire==&Wire  && a.i2c_bus_addr==0x60, "T1 Si5351 a; -> Wire,0x60"); }
  { Si5351 b(0x61);      CHECK(b._wire==&Wire  && b.i2c_bus_addr==0x61, "T1 Si5351 b(0x61); -> Wire,0x61"); }
  { Si5351 c(&Wire1);    CHECK(c._wire==&Wire1 && c.i2c_bus_addr==0x60, "T1 Si5351 c(&Wire1); -> Wire1,0x60"); }
  { Si5351 d(&Wire1,0x61); CHECK(d._wire==&Wire1 && d.i2c_bus_addr==0x61, "T1 Si5351 d(&Wire1,0x61)"); }
  { uint8_t addr=0x62; Si5351 e(addr); CHECK(e._wire==&Wire && e.i2c_bus_addr==0x62, "T1 Si5351 e(uint8_t var)"); }
  { Si5351 *p=new Si5351(&Wire1); CHECK(p->_wire==&Wire1, "T1 new Si5351(&Wire1) (issue #106 line)"); delete p; }
  // T2 routing: chip on Wire1 only
  clear(); Wire1.present=true;
  { Si5351 s(&Wire1); bool r=s.init(SI5351_CRYSTAL_LOAD_8PF,0,0); s.set_freq(1000000000ULL,SI5351_CLK0);
    CHECK(r, "T2 init() true with chip on Wire1");
    CHECK(Wire.log.empty(), "T2 zero traffic on Wire (bus0)");
    CHECK(!Wire1.log.empty() && Wire1.log[0]=="BEGIN", "T2 begin() called on Wire1");
    printf("      Wire1 events=%zu Wire events=%zu\n",Wire1.log.size(),Wire.log.size());
    // T5 status reads on selected bus
    size_t before=Wire1.log.size(); s.update_status();
    bool rxw1=false; for(size_t i=before;i<Wire1.log.size();i++) if(Wire1.log[i].rfind("RX",0)==0) rxw1=true;
    CHECK(rxw1 && Wire.log.empty(), "T5 update_status() reads only on Wire1");
    uint8_t v=s.si5351_read(16); CHECK(v==Wire1.regs[16] && Wire.log.empty(), "T5 si5351_read() returns Wire1 data"); }
  // T4 no-ACK: chip absent on chosen bus (and the #106 failure: default bus while chip on Wire1)
  clear(); Wire1.present=true;
  { Si5351 s; CHECK(!s.init(SI5351_CRYSTAL_LOAD_8PF,0,0) && Wire1.log.empty(), "T4 default-bus object, chip on Wire1 -> init false, Wire1 untouched (old #106 behavior)"); }
  clear();
  { Si5351 s(&Wire1); CHECK(!s.init(SI5351_CRYSTAL_LOAD_8PF,0,0) && Wire.log.empty(), "T4 no ACK on Wire1 -> init false, no bus0 fallback"); }
  // Review #7 additions
  { Si5351 z(0);    CHECK(z._wire==&Wire && z.i2c_bus_addr==0x00, "T1 Si5351(0) -> address path (Wire,0x00)"); }
  { Si5351 n(NULL); CHECK(n._wire==&Wire && n.i2c_bus_addr==0x00, "T1 Si5351(NULL) -> address path (Wire,0x00)"); }
  { Si5351 q(0x61u), r(0x61L), t(0x61UL), u(0x61LL), w(0x61ULL); uint16_t a16=0x61; Si5351 x(a16); char ch=0x61; Si5351 y(ch);
    CHECK(q.i2c_bus_addr==0x61&&r.i2c_bus_addr==0x61&&t.i2c_bus_addr==0x61&&u.i2c_bus_addr==0x61&&w.i2c_bus_addr==0x61&&x.i2c_bus_addr==0x61&&y.i2c_bus_addr==0x61
          && q._wire==&Wire && x._wire==&Wire, "T1 all integer types -> address path"); }
  { TwoWire *np=0; Si5351 g(np); CHECK(g._wire==&Wire, "T1 null TwoWire* variable -> Wire (guard)"); }
#if __cplusplus >= 201103L
  { Si5351 g(nullptr); CHECK(g._wire==&Wire && g.i2c_bus_addr==0x60, "T1 Si5351(nullptr) -> Wire,0x60 (guard)"); }
#endif
  // T6 two objects on two buses stay isolated
  clear(); Wire.present=Wire1.present=true;
  { Si5351 s0, s1(&Wire1);
    bool r0=s0.init(SI5351_CRYSTAL_LOAD_8PF,0,0); size_t w0=Wire.log.size(), w1=Wire1.log.size();
    CHECK(r0 && w1==0, "T6 s0.init() touches Wire only");
    bool r1=s1.init(SI5351_CRYSTAL_LOAD_8PF,0,0); CHECK(r1 && Wire.log.size()==w0, "T6 s1.init() touches Wire1 only");
    s0.set_freq(1000000000ULL,SI5351_CLK0); w0=Wire.log.size(); w1=Wire1.log.size();
    s1.set_freq(2500000000ULL,SI5351_CLK1); CHECK(Wire.log.size()==w0, "T6 s1.set_freq no traffic on Wire");
    size_t w1b=Wire1.log.size(); s0.set_freq(700000000ULL,SI5351_CLK2); CHECK(Wire1.log.size()==w1b, "T6 s0.set_freq no traffic on Wire1");
    CHECK(memcmp(Wire.regs+42,Wire1.regs+42,24)!=0, "T6 Multisynth regs 42..65 differ between buses (independent state)"); }
  printf("%s (%d failures)\n", fails?"FAILED":"ALL PASS", fails); return fails!=0;
}
