// Native host harness: drives the Si5351 library through identical call
// sequences and emits, per scenario, the full I2C transcript, return codes,
// final 256-byte register file and public object state as one JSON line.
// Compiled twice: once against the pristine original, once against the refactor.
#include "si5351.h"
#include "sim.h"
#include <new>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <functional>
#include <signal.h>
#include <setjmp.h>
static sigjmp_buf g_fpe_jb;
static void on_fpe(int) { siglongjmp(g_fpe_jb, 1); }

typedef unsigned long long ull;
static const ull M = SI5351_FREQ_MULT;  // Hz -> library units

alignas(Si5351) static unsigned char objbuf[sizeof(Si5351)];
static Si5351 *S;
static std::vector<std::string> RETS;

static void mark(const char *fmt, ...) {
  char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  g_sim.mark(b);
}
static void ret(const char *name, long long v) {
  char b[96]; snprintf(b, sizeof b, "%s=%lld", name, v);
  RETS.push_back(b); g_sim.events.push_back(std::string("RET ") + b);
}

// ---- API wrappers (mark call, run, record return) ----
static bool init_(uint8_t load, uint32_t xo, int32_t corr) {
  mark("init(%u,%u,%d)", load, xo, corr); bool r = S->init(load, xo, corr); ret("init", r); return r; }
static void reset_() { mark("reset()"); S->reset(); }
static uint8_t sf(ull f, int clk) {
  mark("set_freq(%llu,CLK%d)", f, clk); uint8_t r = S->set_freq(f, (enum si5351_clock)clk); ret("set_freq", r); return r; }
static uint8_t sfm(ull f, ull p, int clk) {
  mark("set_freq_manual(%llu,%llu,CLK%d)", f, p, clk);
  uint8_t r = S->set_freq_manual(f, p, (enum si5351_clock)clk); ret("set_freq_manual", r); return r; }
static void spll(ull p, int pll) { mark("set_pll(%llu,PLL%c)", p, pll ? 'B' : 'A'); S->set_pll(p, (enum si5351_pll)pll); }
static void sms(int clk, uint32_t p1, uint32_t p2, uint32_t p3, int im, int rd, int d4) {
  mark("set_ms(CLK%d,{%u,%u,%u},%d,%d,%d)", clk, p1, p2, p3, im, rd, d4);
  struct Si5351RegSet r; r.p1 = p1; r.p2 = p2; r.p3 = p3;
  S->set_ms((enum si5351_clock)clk, r, im, rd, d4); }
static void svcxo(ull p, int ppm) { mark("set_vcxo(%llu,%d)", p, ppm); S->set_vcxo(p, ppm); }
static void corr_(int32_t c, int ref) { mark("set_correction(%d,%d)", c, ref); S->set_correction(c, (enum si5351_pll_input)ref); }
static void mssrc(int clk, int pll) { mark("set_ms_source(CLK%d,PLL%c)", clk, pll ? 'B' : 'A'); S->set_ms_source((enum si5351_clock)clk, (enum si5351_pll)pll); }
static void oe(int clk, int en) { mark("output_enable(CLK%d,%d)", clk, en); S->output_enable((enum si5351_clock)clk, en); }
static void pllin(int pll, int in) { mark("set_pll_input(%d,%d)", pll, in); S->set_pll_input((enum si5351_pll)pll, (enum si5351_pll_input)in); }
static void reff(uint32_t f, int ref) { mark("set_ref_freq(%u,%d)", f, ref); S->set_ref_freq(f, (enum si5351_pll_input)ref); }
static void pllrst(int pll) { mark("pll_reset(%d)", pll); S->pll_reset((enum si5351_pll)pll); }
static void sph(int clk, int ph) { mark("set_phase(CLK%d,%d)", clk, ph); S->set_phase((enum si5351_clock)clk, ph); }
static void sint(int clk, int en) { mark("set_int(CLK%d,%d)", clk, en); S->set_int((enum si5351_clock)clk, en); }
static void drv(int clk, int d) { mark("drive_strength(CLK%d,%d)", clk, d); S->drive_strength((enum si5351_clock)clk, (enum si5351_drive)d); }
// Si5351_FreqChecks.ino: one check = [reset(); allOff();] drive_strength(8mA); set_freq(f*100); output_enable(clk,1)
static const struct { ull hz; int clk; } FC[12] = {
  {10000000ULL,0},{14070000ULL,0},{28000000ULL,0},{100000000ULL,0},{112500000ULL,0},{150000000ULL,0},
  {200000000ULL,0},{210000000ULL,0},{512000ULL,0},{4000ULL,0},{23000000ULL,6},{87400000ULL,7}};
static void fc_check(int i) {
  mark("FreqCheck #%d", i + 1);
  if (FC[i].clk != 7) { reset_(); for (int c = 0; c < 8; c++) oe(c, 0); }
  drv(FC[i].clk, SI5351_DRIVE_8MA);
  sf(FC[i].hz * 100ULL, FC[i].clk);
  oe(FC[i].clk, 1);
}
static void misc_smoke() {
  mark("misc_smoke");
  S->drive_strength(SI5351_CLK2, SI5351_DRIVE_8MA);
  S->set_phase(SI5351_CLK1, 0xFF);
  S->set_int(SI5351_CLK3, 1);
  S->set_clock_pwr(SI5351_CLK4, 0);
  S->set_clock_invert(SI5351_CLK5, 1);
  S->set_clock_source(SI5351_CLK1, SI5351_CLK_SRC_MS0);
  S->set_clock_disable(SI5351_CLK6, SI5351_CLK_DISABLE_HI_Z);
  S->set_clock_fanout(SI5351_FANOUT_XO, 1);
  S->update_status();
  ret("get_correction0", S->get_correction(SI5351_PLL_INPUT_XO));
}
static const uint8_t LOAD = SI5351_CRYSTAL_LOAD_8PF;
// ---- Si5351_Issue65_Verify.ino replica ----
static void hv_fresh() { reset_(); for (int c = 0; c < 8; c++) oe(c, 0); for (int c = 0; c < 6; c++) drv(c, SI5351_DRIVE_8MA); }
static void hv_dump(int clk) { mark("dump(CLK%d)", clk); S->update_status(); S->si5351_read(16 + clk); S->si5351_read(44 + 8 * clk);
  S->si5351_read(44 + 8 * clk); S->si5351_read(3); }
static void hv_step(int s) {
  mark("STEP %d", s);
  switch (s) {
  case 1: hv_fresh(); sf(21000000000ULL, 0); oe(0, 1); hv_fresh(); sf(51200000ULL, 0); oe(0, 1); hv_dump(0); break;
  case 2: hv_fresh(); oe(0, 1); sf(8000000000ULL, 0); sf(15600000000ULL, 0); sf(8000000000ULL, 0); hv_dump(0); break;
  case 3: hv_fresh(); oe(0, 1); sf(15500000000ULL, 0); sf(9600000000ULL, 0); hv_dump(0); break;
  case 4: hv_fresh(); oe(0, 1); sf(20000000000ULL, 0); sf(1000000000ULL, 0); hv_dump(0); break;
  case 5: hv_fresh(); mssrc(1, 1); oe(1, 1); sf(16000000000ULL, 1); sf(1400000000ULL, 1); hv_dump(1); break;
  case 6: hv_fresh(); oe(2, 1); sfm(17500000000ULL, 70000000000ULL, 2); sfm(1400000000ULL, 80000000000ULL, 2); hv_dump(2); break;
  case 7: hv_fresh(); oe(0, 1); for (ull i = 0; i <= 1000; i++) sf((7000000ULL + 10ULL * i) * 100ULL, 0); hv_dump(0); break;
  case 8: hv_fresh(); oe(0, 1); oe(2, 1); sf(15500000000ULL, 0); sf(9900000000ULL, 2); hv_dump(2); break;
  }
}

static void init0() { init_(LOAD, 0, 0); }

// ---- Si5351_Issue65_Verify.ino v2 replica (steps with controls); keep in sync with the sketch ----
static const char *HV2_NAMES[] = {"1","2","3","4","5c1","5c2","5","5x","6c","6","6x","7","8","9"};
static const int HV2_N = 14;
static void hv2_dump(int clk) { mark("dump(CLK%d)", clk); S->update_status(); S->si5351_read(3); S->si5351_read(16 + clk); }
static void hv2_step(int s) {
  mark("STEP %s", HV2_NAMES[s]);
  int clk = 0;
  switch (s) {
  case 0: hv_fresh(); sf(21000000000ULL, 0); oe(0, 1); hv_fresh(); sf(51200000ULL, 0); oe(0, 1); break;
  case 1: hv_fresh(); oe(0, 1); sf(8000000000ULL, 0); sf(15600000000ULL, 0); sf(8000000000ULL, 0); break;
  case 2: hv_fresh(); oe(0, 1); sf(15500000000ULL, 0); sf(9600000000ULL, 0); break;
  case 3: hv_fresh(); oe(0, 1); sf(20000000000ULL, 0); sf(1000000000ULL, 0); break;
  case 4: clk = 1; hv_fresh(); oe(1, 1); sf(1400000000ULL, 1); break;                                   // control: CLK1, PLLA, no DIVBY4
  case 5: clk = 1; hv_fresh(); mssrc(1, 1); oe(1, 1); sf(1400000000ULL, 1); break;                      // control: CLK1, PLLB, no DIVBY4
  case 6: clk = 1; hv_fresh(); mssrc(1, 1); oe(1, 1); sf(16000000000ULL, 1); sf(1400000000ULL, 1); break;
  case 7: clk = 0; hv_fresh(); mssrc(0, 1); oe(0, 1); sf(16000000000ULL, 0); sf(1400000000ULL, 0); break; // step 5 on CLK0
  case 8: clk = 2; hv_fresh(); oe(2, 1); sfm(1400000000ULL, 80000000000ULL, 2); break;                 // control: CLK2 manual
  case 9: clk = 2; hv_fresh(); oe(2, 1); sfm(17500000000ULL, 70000000000ULL, 2); sfm(1400000000ULL, 80000000000ULL, 2); break;
  case 10: clk = 0; hv_fresh(); oe(0, 1); sfm(17500000000ULL, 70000000000ULL, 0); sfm(1400000000ULL, 80000000000ULL, 0); break; // step 6 on CLK0
  case 11: hv_fresh(); oe(0, 1); for (ull i = 0; i <= 1000; i++) sf((7000000ULL + 10ULL * i) * 100ULL, 0); break;
  case 12: clk = 2; hv_fresh(); oe(0, 1); oe(2, 1); sf(15500000000ULL, 0); sf(9900000000ULL, 2); break;
  case 13: hv_fresh(); oe(0, 1); sf(12000000000ULL, 0); break;                                          // 120 MHz: MS ratio 6, MS_INT=0
  }
  hv2_dump(clk);
}

// ---- Si5351_Issue65_Verify.ino v3 replica: steps + list of clocks to probe/check; keep in sync with gen_verify_sketch.py ----
struct HV3 { const char *name; int clks[3]; };
static const HV3 HV3S[] = {
  {"1",{0,-1,-1}}, {"2",{0,-1,-1}}, {"3",{0,-1,-1}}, {"4",{0,-1,-1}}, {"5c1",{1,-1,-1}}, {"5c2",{1,-1,-1}}, {"5",{1,-1,-1}},
  {"5x",{0,-1,-1}}, {"6c",{2,-1,-1}}, {"6",{2,-1,-1}}, {"6x",{0,-1,-1}}, {"7",{0,-1,-1}}, {"8",{2,0,-1}}, {"9",{0,-1,-1}},
  {"10",{2,1,0}}, {"11",{2,0,-1}}, {"12",{2,0,-1}}, {"13",{2,0,-1}}, {"14",{0,-1,-1}}, {"15",{2,1,0}}};
static const int HV3_N = sizeof(HV3S) / sizeof(HV3S[0]);
static void hv3_step(int s) {
  mark("STEP %s", HV3S[s].name);
  switch (s) {
  case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 13:
    { int map2[] = {0,1,2,3,4,5,6,7,8,9,10,11,-1,13}; int s2 = map2[s];
      // reuse v2 sequences without their dump
      switch (s2) {
      case 0: hv_fresh(); sf(21000000000ULL, 0); oe(0, 1); hv_fresh(); sf(51200000ULL, 0); oe(0, 1); break;
      case 1: hv_fresh(); oe(0, 1); sf(8000000000ULL, 0); sf(15600000000ULL, 0); sf(8000000000ULL, 0); break;
      case 2: hv_fresh(); oe(0, 1); sf(15500000000ULL, 0); sf(9600000000ULL, 0); break;
      case 3: hv_fresh(); oe(0, 1); sf(20000000000ULL, 0); sf(1000000000ULL, 0); break;
      case 4: hv_fresh(); oe(1, 1); sf(1400000000ULL, 1); break;
      case 5: hv_fresh(); mssrc(1, 1); oe(1, 1); sf(1400000000ULL, 1); break;
      case 6: hv_fresh(); mssrc(1, 1); oe(1, 1); sf(16000000000ULL, 1); sf(1400000000ULL, 1); break;
      case 7: hv_fresh(); mssrc(0, 1); oe(0, 1); sf(16000000000ULL, 0); sf(1400000000ULL, 0); break;
      case 8: hv_fresh(); oe(2, 1); sfm(1400000000ULL, 80000000000ULL, 2); break;
      case 9: hv_fresh(); oe(2, 1); sfm(17500000000ULL, 70000000000ULL, 2); sfm(1400000000ULL, 80000000000ULL, 2); break;
      case 10: hv_fresh(); oe(0, 1); sfm(17500000000ULL, 70000000000ULL, 0); sfm(1400000000ULL, 80000000000ULL, 0); break;
      case 11: hv_fresh(); oe(0, 1); for (ull i = 0; i <= 1000; i++) sf((7000000ULL + 10ULL * i) * 100ULL, 0); break;
      case 13: hv_fresh(); oe(0, 1); sf(12000000000ULL, 0); break;
      } } break;
  case 12: hv_fresh(); oe(0, 1); oe(2, 1); sf(15500000000ULL, 0); sf(9900000000ULL, 2); break;            // #65 3rd: CLK2 moves to PLLB
  case 14: hv_fresh(); mssrc(1, 1); oe(0, 1); oe(1, 1); oe(2, 1); sf(1400000000ULL, 1);                  // PLLB shared with CLK1 14 MHz: reused
           sf(15500000000ULL, 0); sf(9900000000ULL, 2); break;
  case 15: hv_fresh(); spll(60000000000ULL, 1); oe(0, 1); oe(2, 1); sf(15500000000ULL, 0); sf(9900000000ULL, 2); break; // PLLB free but 600: re-planned
  case 16: hv_fresh(); oe(2, 1); sf(16000000000ULL, 2); hv_fresh(); oe(0, 1); oe(2, 1);                  // move + DIVBY4 exit
           sf(15500000000ULL, 0); sf(9900000000ULL, 2); break;
  case 17: hv_fresh(); oe(0, 1); oe(2, 1); sf(15500000000ULL, 0); sf(9900000000ULL, 2); sf(1000000000ULL, 0); // back and forth
           sf(9800000000ULL, 2); sf(16000000000ULL, 0); sf(9700000000ULL, 2); break;
  case 18: hv_fresh(); oe(0, 1); sf(20000000000ULL, 0); sf(11000000000ULL, 0); break;                    // single reset (dedupe)
  case 19: hv_fresh(); mssrc(1, 1); oe(0, 1); oe(1, 1); oe(2, 1); sf(16000000000ULL, 1);                 // both PLLs held: fallback
           sf(15500000000ULL, 0); sf(9900000000ULL, 2); break;
  }
  for (int j = 0; j < 3 && HV3S[s].clks[j] >= 0; j++) { mark("dump(CLK%d)", HV3S[s].clks[j]); S->update_status(); S->si5351_read(3); S->si5351_read(16 + HV3S[s].clks[j]); }
}

// ---- deterministic PRNG for fuzz ----
static ull rng_state;
static ull rnd() { ull z = (rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static ull rr(ull lo, ull hi) { return lo + rnd() % (hi - lo + 1); }

static std::vector<ull> edge_freqs() {
  std::vector<ull> v;
  ull t[] = {400000ULL, 1845400ULL};  // CLKOUT_MIN*100, CLKOUT67_MIN*100
  for (ull base : t) for (int k = 0; k <= 7; k++) { ull e = base << k; v.push_back(e - 1); v.push_back(e); v.push_back(e + 1); }
  ull more[] = {1, 399999, 50000000ULL, 51200000ULL, 10000000000ULL - 1, 10000000000ULL, 10000000001ULL,
                15000000000ULL - 1, 15000000000ULL, 15000000001ULL, 22500000000ULL, 22500000001ULL,
                1400000000ULL, 1407400000ULL, 2300000000ULL, 8740000000ULL, 11200000000ULL};
  for (ull m : more) v.push_back(m);
  return v;
}
static ull rand_freq(bool clk67) {
  static std::vector<ull> E = edge_freqs();
  ull f;
  if (rnd() % 3 == 0) f = E[rnd() % E.size()];
  else { // log-uniform 1 kHz .. 250 MHz
    double lo = 3.0, hi = 8.4, x = lo + (hi - lo) * (double)(rnd() % 1000000) / 1e6;
    f = (ull)(__builtin_pow(10.0, x) * 100.0);
  }
  if (f == 0) f = 1;
  (void)clk67;
  return f;
}
static void fuzz_seq(ull seed, int n_ops) {
  rng_state = seed;
  init_(LOAD, (rnd() % 4 == 0) ? 27000000 : 0, (rnd() % 3 == 0) ? (int32_t)rr(0, 400000) - 200000 : 0);
  for (int i = 0; i < n_ops; i++) {
    int op = rnd() % 100;
    if (op < 45) sf(rand_freq(false), rnd() % 8);
    else if (op < 58) sfm(rand_freq(false), rr(55000, 95000) * 1000000ULL, rnd() % 8);
    else if (op < 66) mssrc(rnd() % 8, rnd() % 2);
    else if (op < 72) corr_((int32_t)rr(0, 400000) - 200000, rnd() % 2);
    else if (op < 77) svcxo(rr(60000, 90000) * 1000000ULL, (int)rr(0, 300));
    else if (op < 82) spll(rr(55000, 95000) * 1000000ULL, rnd() % 2);
    else if (op < 89) sms(rnd() % 8, rnd() & 0x3FFFF, rnd() & 0xFFFFF, (rnd() & 0xFFFFF) | 1, rnd() % 2, rnd() % 8, rnd() % 2);
    else if (op < 93) oe(rnd() % 8, rnd() % 2);
    else if (op < 95) reset_();
    else if (op < 97) pllrst(rnd() % 2);
    else { reff(rnd() % 2 ? 25000000 : (uint32_t)rr(10000000, 100000000), 1); pllin(rnd() % 2, rnd() % 2); }
  }
}

// ---- scenarios ----
struct Scn { std::string id; std::function<void()> fn; bool present; };
static std::vector<Scn> SC;
static void add(const std::string &id, std::function<void()> fn, bool present = true) { SC.push_back({id, fn, present}); }

static void build() {
  // ===== Golden cases (golden_cases.json, 27 ids) =====
  struct { const char *id; ull f; int32_t corr; } g05[] = {
    {"P0-clk0-common-1-14MHz", 14000000, 0}, {"P0-clk0-common-2-10MHz", 10000000, 0},
    {"P0-clk0-common-3-7MHz", 7000000, 0}, {"P0-clk0-common-4-28MHz", 28000000, 0},
    {"P0-clk0-integer-MS-16MHz", 16000000, 0}, {"P0-clk0-fractional-MS-14p07MHz", 14070000, 0},
    {"P0-clk0-gt100MHz-recalc-pll-112MHz", 112000000, 0}, {"P0-clk0-DIVBY4-boundary-150MHz", 150000000, 0},
    {"P0-clk0-DIVBY4-200MHz", 200000000, 0}, {"P0-corr-100000-14MHz-existing-pll", 14000000, 100000},
    {"P0-corr--50000-14MHz-existing-pll", 14000000, -50000}, {"P0-corr-100000-gt100MHz-112MHz", 112000000, 100000},
    {"P0-rdiv-128-5kHz", 5000, 0}, {"P0-rdiv-64-10kHz", 10000, 0}, {"P0-rdiv-8-100kHz", 100000, 0},
    {"P0-rdiv-2-400kHz", 400000, 0}, {"P0-rdiv-1-512kHz-boundary", 512000, 0},
  };
  for (auto &g : g05) { ull f = g.f * M; int32_t c = g.corr; add(std::string("G:") + g.id, [f, c] { init_(LOAD, 0, c); sf(f, 0); }); }
  add("G:P0-manual-DIVBY4-175MHz-pll700", [] { init0(); mssrc(1, 1); sfm(175000000 * M, 700000000 * M, 1); });
  add("G:P0-clk1-PLLB-14MHz", [] { init0(); mssrc(1, 1); sf(14000000 * M, 1); });
  add("G:P0-clk6-23MHz", [] { init0(); sf(23000000 * M, 6); });
  add("G:P0-clk7-874MHz-even-div-of-clk6-pll", [] { init0(); sf(23000000 * M, 6); sf(87400000 * M, 7); });
  add("G:P0-clk7-reject-noninteger-12p345678MHz", [] { init0(); sf(23000000 * M, 6); sf(12345678 * M, 7); });
  add("G:P0-vcxo-876MHz-40ppm", [] { init0(); svcxo(876000000 * M, 40); });
  add("G:P0-vcxo-manual-146MHz-on-876pll", [] { init0(); svcxo(876000000 * M, 40); mssrc(0, 1); sfm(146000000 * M, 876000000 * M, 0); });
  add("G:P1-parity-set_freq-vs-manual-14MHz-pll800#auto", [] { init0(); sf(14000000 * M, 0); });
  add("G:P1-parity-set_freq-vs-manual-14MHz-pll800#manual", [] { init0(); sfm(14000000 * M, 800000000 * M, 0); });
  add("G:P1-parity-gt100-set_freq-vs-manual-175MHz#auto", [] { init0(); sf(175000000 * M, 0); });
  add("G:P1-parity-gt100-set_freq-vs-manual-175MHz#manual", [] { init0(); sfm(175000000 * M, 700000000 * M, 0); });
  add("G:P1-reset-pll-fixed-800MHz", [] { init0(); });

  // ===== Example sketches verbatim =====
  add("X:example-si5351_example", [] { init0(); sf(1400000000ULL, 0); mssrc(1, 1); sfm(17500000000ULL, 70000000000ULL, 1); S->update_status(); });
  add("X:example-clk67", [] { init0(); sf(1400000000ULL, 0); sf(2300000000ULL, 6); sf(1234567800ULL, 7); sf(8740000000ULL, 7); });
  add("X:example-vcxo", [] { init0(); svcxo(87600000000ULL, 40); mssrc(0, 1); sfm(14600000000ULL, 87600000000ULL, 0); });

  // ===== P1 multi-clock >100 MHz conflicts / share =====
  add("X:conflict-clk1-gt100-same-pll", [] { init0(); sf(112000000 * M, 0); sf(120000000 * M, 1); });
  add("X:conflict-clk1-gt100-other-pll-ok", [] { init0(); sf(112000000 * M, 0); mssrc(1, 1); sf(120000000 * M, 1); });
  add("X:conflict-same-clk-twice-gt100", [] { init0(); sf(112000000 * M, 0); sf(130000000 * M, 0); });
  add("X:share-peer-recalc", [] { init0(); sf(10000000 * M, 1); sf(7100000 * M, 2); sf(5000 * M, 3); sf(112000000 * M, 0); sf(14000000 * M, 4); });
  add("X:share-peer-recalc-divby4", [] { init0(); sf(10000000 * M, 1); sf(160000000 * M, 0); sf(3000000 * M, 5); });
  add("X:share-boundary-100MHz", [] { init0(); sf(100000000 * M, 0); sf(100000000 * M + 1, 1); sf(100000000 * M - 1, 2); });
  add("X:share-pllb-gt100", [] { init0(); mssrc(2, 1); mssrc(3, 1); sf(20000000 * M, 3); sf(140000000 * M, 2); sf(110000000 * M, 0); });
  add("X:all-six-clocks", [] { init0(); for (int c = 0; c < 6; c++) sf((ull)(1000000 + 3333333 * c) * M, c); });

  // ===== DIVBY4 / bounds =====
  add("X:divby4-edges", [] { init0(); sf(150000000 * M - 1, 0); sf(150000000 * M, 1); sf(150000000 * M + 1, 2); });
  add("X:bounds-clk05", [] { init0(); sf(0, 0); sf(1, 1); sf(399999, 2); sf(400000, 3); sf(22500000000ULL, 4); sf(30000000000ULL, 5); });
  add("X:bounds-manual", [] { init0(); sfm(1, 80000000000ULL, 0); sfm(30000000000ULL, 90000000000ULL, 1); sfm(16000000000ULL, 64000000000ULL, 2); });

  // ===== R-div band edges (exact thresholds and +-1), CLK0-5 and CLK6/7 =====
  for (int k = 0; k <= 7; k++) {
    char id[64]; snprintf(id, sizeof id, "X:rdiv05-edge-k%d", k);
    ull e = 400000ULL << k;
    add(id, [e] { init0(); sf(e - 1, 0); sf(e, 1); sf(e + 1, 2); sfm(e - 1, 80000000000ULL, 3); sfm(e, 80000000000ULL, 4); sfm(e + 1, 87600000000ULL, 5); });
    snprintf(id, sizeof id, "X:rdiv67-edge-k%d", k);
    ull e7 = 1845400ULL << k;
    add(id, [e7] { init0(); sf(e7 - 1, 6); });
    snprintf(id, sizeof id, "X:rdiv67-edge-k%d-eq", k);
    add(id, [e7] { init0(); sf(e7, 7); });
    snprintf(id, sizeof id, "X:rdiv67-edge-k%d-p1", k);
    add(id, [e7] { init0(); sf(e7 + 1, 6); });
  }

  // ===== CLK6 / CLK7 peer logic =====
  add("X:clk7-first-then-clk6-accept", [] { init0(); sf(23000000 * M, 7); sf(87400000 * M, 6); });
  add("X:clk7-first-then-clk6-reject-odd", [] { init0(); sf(23000000 * M, 7); sf(46000000 * M, 6); });
  add("X:clk7-first-then-clk6-reject-nonint", [] { init0(); sf(23000000 * M, 7); sf(12345678 * M, 6); });
  add("X:clk6-then-clk7-reject-odd", [] { init0(); sf(23000000 * M, 6); sf(46000000 * M, 7); });
  add("X:clk6-then-clk7-even-lowfreq-ms67fail", [] { init0(); sf(23000000 * M, 6); sf(23000 * M, 7); });
  add("X:clk6-reset-peer-by-reset", [] { init0(); sf(23000000 * M, 6); reset_(); sf(12345678 * M, 7); });
  add("X:clk67-bounds", [] { init0(); sf(1, 6); sf(1845399, 7); });
  add("X:clk67-upper-clamp", [] { init0(); sf(20000000000ULL, 6); });
  add("X:clk67-upper-clamp7", [] { init0(); sf(15000000000ULL, 7); });
  add("X:clk67-resets-6", [] { init0(); sf(10000000 * M, 6); sf(20000000 * M, 6); });
  add("X:clk6-pll-asym-clk6-on-plla", [] { init0(); mssrc(6, 0); sf(23000000 * M, 6); });
  add("X:clk7-pll-asym-clk7-on-plla", [] { init0(); mssrc(7, 0); sf(23000000 * M, 7); });
  add("X:clk67-plus-clk0", [] { init0(); sf(14000000 * M, 0); sf(30000000 * M, 6); sf(60000000 * M, 7); sf(7000000 * M, 1); });

  // ===== set_freq_manual =====
  add("X:manual-various", [] { init0(); sfm(14000000 * M, 87600000000ULL, 0); sfm(5000 * M, 80000000000ULL, 1); sfm(200000000 * M, 80000000000ULL, 2);
                               mssrc(3, 1); sfm(10000000 * M, 60000000000ULL, 3); sfm(1000000 * M, 90000000000ULL, 5); });
  add("X:manual-on-clk6-clk7", [] { init0(); sfm(20000000 * M, 80000000000ULL, 6); sfm(40000000 * M, 80000000000ULL, 7); });
  add("X:manual-pll-out-of-vco", [] { init0(); sfm(10000000 * M, 50000000000ULL, 0); sfm(10000000 * M, 100000000000ULL, 1); });

  // ===== correction =====
  add("X:corr-then-freq", [] { init0(); sf(14000000 * M, 0); corr_(100000, 0); sf(112000000 * M, 0); sf(10000000 * M, 1); corr_(-50000, 0); sf(7000000 * M, 2); });
  add("X:corr-clkin", [] { init0(); reff(50000000, 1); pllin(1, 1); corr_(-30000, 1); sf(23000000 * M, 6); sf(14000000 * M, 0); mssrc(1, 1); sf(10000000 * M, 1); });
  add("X:corr-init-large", [] { init_(LOAD, 0, 1500000); sf(14000000 * M, 0); sf(150000000 * M, 1); });
  add("X:clkin-dividers", [] { init0(); reff(25000000, 1); pllin(0, 1); sf(112000000 * M, 0); reff(80000000, 1); pllin(0, 1); sf(112000000 * M, 0); pllin(0, 0); });

  // ===== VCXO =====
  add("X:vcxo-ppm-bounds", [] { init0(); svcxo(87600000000ULL, 10); svcxo(87600000000ULL, 30); svcxo(87600000000ULL, 240); svcxo(87600000000ULL, 255); });
  add("X:vcxo-pll-range", [] { init0(); svcxo(60000000000ULL, 100); svcxo(90000000000ULL, 100); svcxo(70012345600ULL, 77); svcxo(50000000000ULL, 50); });
  add("X:vcxo-with-corr", [] { init_(LOAD, 0, 12345); svcxo(87600000000ULL, 40); mssrc(0, 1); sfm(14600000000ULL, 87600000000ULL, 0); });

  // ===== reset/init =====
  add("X:init-27MHz", [] { init_(LOAD, 27000000, 0); sf(14000000 * M, 0); sf(23000000 * M, 6); });
  add("X:init-xo-50MHz", [] { init_(SI5351_CRYSTAL_LOAD_10PF, 50000000, 0); sf(14000000 * M, 0); });
  add("X:init-xo-150MHz-ignored", [] { init_(SI5351_CRYSTAL_LOAD_6PF, 150000000, 0); sf(14000000 * M, 0); });
  add("X:reset-after-use", [] { init0(); sf(14000000 * M, 0); sf(112000000 * M, 1); sf(23000000 * M, 6); mssrc(2, 1); reset_(); sf(14000000 * M, 2); });
  add("X:init-no-device", [] { init0(); sf(14000000 * M, 0); }, false);

  // ===== direct set_ms / set_pll public calls =====
  add("X:set_ms-direct-all-clks", [] { init0();
    for (int c = 0; c < 8; c++) sms(c, 0x3ABCD + c, 0xFEDCB - c, 0xF4240 + c, c & 1, (c * 3) & 7, (c >> 1) & 1); });
  add("X:set_ms-direct-maxbits", [] { init0();
    for (int c = 0; c < 8; c++) sms(c, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1, 7, 1); });
  add("X:set_pll-direct", [] { init0(); spll(0, 0); spll(87654321012ULL, 1); spll(200000000000ULL, 0); spll(64000000000ULL, 1); });
  add("X:misc-smoke", [] { init0(); misc_smoke(); oe(3, 1); oe(3, 0); pllrst(0); pllrst(1); });

#ifdef OOR_SCENARIOS
  // Out-of-contract: clk value outside enum si5351_clock (not part of pass/fail set)
  add("OOR:set_ms-clk8", [] { init0(); sms(8, 0x12345, 0x6789A, 0xF4240, 0, 3, 0); });
  add("OOR:set_freq-clk8", [] { init0(); sf(14000000 * M, 8); });
  add("OOR:set_freq_manual-clk8", [] { init0(); sfm(14000000 * M, 80000000000ULL, 8); });
  return;
#endif
  // ===== extra golden cases with library register maps (generated) =====
#if __has_include("golden_extra.inc")
#include "golden_extra.inc"
#endif

  // ===== Si5351_FreqChecks.ino hardware-check sequences (2026-10-07) =====
  add("H:check9-isolated", [] { init0(); fc_check(8); });            // init -> check 9 only
  add("H:check10-isolated", [] { init0(); fc_check(9); });
  add("H:check8-then-9", [] { init0(); fc_check(7); fc_check(8); });
  add("H:check11-isolated", [] { init0(); fc_check(10); });
  for (int k = 1; k <= 12; k++) {                                    // real sketch: checks 1..k cumulative
    char id[40]; snprintf(id, sizeof id, "H:freqchecks-1-to-%02d", k);
    add(id, [k] { init0(); for (int i = 0; i < k; i++) fc_check(i); });
  }

  // ===== Issue #65 scenarios (DIVBY4 exit), 2026-10-07 =====
  add("I65:S1-clk0-80-156-80", [] { init0(); sf(80000000 * M, 0); sf(156000000 * M, 0); sf(80000000 * M, 0); });
  add("I65:S2-clk0-80-156-143-80-control", [] { init0(); sf(80000000 * M, 0); sf(156000000 * M, 0); sf(143000000 * M, 0); sf(80000000 * M, 0); });
  add("I65:S3-clk0-80-156-80-manual-pll_reset", [] { init0(); sf(80000000 * M, 0); sf(156000000 * M, 0); sf(80000000 * M, 0); pllrst(0); });
  add("I65:S4-clk0-155-96", [] { init0(); sf(155000000 * M, 0); sf(96000000 * M, 0); });
  add("I65:S6-clk0-155-then-clk2-99", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("I65:clk0-200-10", [] { init0(); sf(200000000 * M, 0); sf(10000000 * M, 0); });
  add("I65:clk0-200-512k-rdiv", [] { init0(); sf(200000000 * M, 0); sf(512000 * M, 0); sf(5000 * M, 0); });
  add("I65:clk0-200-120-gt100-path", [] { init0(); sf(200000000 * M, 0); sf(120000000 * M, 0); });
  add("I65:clk0-200-180-stays-divby4", [] { init0(); sf(200000000 * M, 0); sf(180000000 * M, 0); sf(160000000 * M, 0); });
  for (int c = 0; c < 6; c++) {                         // every MS0..5 on both PLLs
    for (int pll = 0; pll < 2; pll++) {
      char id[48]; snprintf(id, sizeof id, "I65:clk%d-pll%c-160-14", c, pll ? 'B' : 'A');
      add(id, [c, pll] { init0(); mssrc(c, pll); sf(160000000 * M, c); sf(14000000 * M, c); });
    }
  }
  add("I65:manual-175-pll700-to-14-pll800-pllb", [] { init0(); mssrc(1, 1); sfm(175000000 * M, 700000000 * M, 1); sfm(14000000 * M, 800000000 * M, 1); });
  add("I65:manual-200-to-set_freq-10", [] { init0(); sfm(200000000 * M, 800000000 * M, 2); sf(10000000 * M, 2); });
  add("I65:set_freq-200-to-manual-10", [] { init0(); sf(200000000 * M, 3); sfm(10000000 * M, 800000000 * M, 3); });
  add("I65:set_ms-direct-divby4-exit", [] { init0(); sms(2, 0, 0, 1, 1, 0, 1); sms(2, 0x0B40, 0, 1, 0, 0, 0); sms(2, 0x0B40, 0, 1, 0, 0, 0); });
  add("I65:reset-after-divby4-then-512k", [] { init0(); sf(210000000 * M, 0); reset_(); sf(512000 * M, 0); oe(0, 1); });
  add("I65:reset-after-divby4-then-14M-other-clk", [] { init0(); sf(210000000 * M, 0); reset_(); sf(14000000 * M, 1); sf(14000000 * M, 0); });
  add("I65:peer-divby4-recalc", [] { init0(); sf(10000000 * M, 1); sf(160000000 * M, 0); sf(3000000 * M, 5); sf(7000000 * M, 0); });

  // ===== Steady-state tuning (no DIVBY4 transition): fix must add ZERO resets =====
  add("T:vfo-clk0-7MHz-1kHz-x300", [] { init0(); for (int i = 0; i <= 300; i++) sf((7000000ULL + 1000ULL * i) * M, 0); });
  add("T:vfo-clk0-14MHz-frac-1.37Hz-x300", [] { init0(); for (int i = 0; i <= 300; i++) sf(14074000ULL * M + 137ULL * i, 0); });
  add("T:vfo-clk0-sweep-3k-100M-log-x400", [] { init0(); for (int i = 0; i <= 400; i++) sf((ull)(__builtin_pow(10.0, 3.5 + 4.5 * i / 400.0) * 100.0), 0); });
  add("T:vfo-clk0-sweep-100M-down-3k-x400", [] { init0(); for (int i = 400; i >= 0; i--) sf((ull)(__builtin_pow(10.0, 3.5 + 4.5 * i / 400.0) * 100.0), 0); });
  add("T:rdiv-band-hop-clk0-clk3", [] { init0();
    ull f[] = {5000, 10000, 20000, 50000, 100000, 200000, 400000, 512000, 1000000, 512000, 400000, 100000, 10000, 4000, 5000, 1000000, 4000};
    for (int r = 0; r < 3; r++) for (ull x : f) { sf(x * M, 0); sf(x * M + 1, 3); } });
  add("T:multi-clk012-interleaved-x150", [] { init0(); for (int i = 0; i < 150; i++) { sf((3500000ULL + 997ULL * i) * M, 0); sf((7000000ULL + 1501ULL * i) * M, 1); sf((50000000ULL + 12345ULL * i) * M, 2); } });
  add("T:clk1-pllb-x200", [] { init0(); mssrc(1, 1); for (int i = 0; i < 200; i++) sf((10000000ULL + 2500ULL * i) * M + i, 1); });
  add("T:clk67-steps", [] { init0(); for (int i = 0; i < 60; i++) { sf((20000000ULL + 100000ULL * i) * M, 6); sf((20000000ULL + 100000ULL * i) * M, 7); } });
  add("T:manual-clk0-pll800-x200", [] { init0(); for (int i = 0; i < 200; i++) sfm((14000000ULL + 50ULL * i) * M, 800000000ULL * M, 0); });
  add("T:manual-clk2-pll-varies-x100", [] { init0(); for (int i = 0; i < 100; i++) sfm(10000000ULL * M, (600000000ULL + 2000000ULL * i) * M, 2); });
  add("T:corr-during-tuning", [] { init0(); for (int i = 0; i < 100; i++) { if (i % 10 == 0) corr_((i - 50) * 1000, 0); sf((7040000ULL + 10ULL * i) * M, 0); } });
  add("T:gt100-non-divby4-tuning-x100", [] { init0(); for (int i = 0; i < 100; i++) sf((101000000ULL + 480000ULL * i) * M, 0); });
  add("T:divby4-steady-tuning-x50", [] { init0(); for (int i = 0; i < 50; i++) sf((151000000ULL + 1000000ULL * i) * M, 0); });
  add("T:repeat-same-freq-x100", [] { init0(); for (int i = 0; i < 100; i++) sf(14070000ULL * M, 0); });
  add("T:freqchecks-1-to-4-then-tune", [] { init0(); for (int i = 0; i < 4; i++) fc_check(i); for (int i = 0; i < 100; i++) sf((28000000ULL + 100ULL * i) * M, 0); });

  // ===== Si5351_Issue65_Verify.ino, call-for-call (steps run cumulatively on one chip) =====
  for (int k = 1; k <= 8; k++) {
    char id[40]; snprintf(id, sizeof id, "HV:verify-steps-1-to-%d", k);
    add(id, [k] { init0(); for (int s = 1; s <= k; s++) hv_step(s); });
  }

  add("HV2:verify-v2-all-steps", [] { init0(); for (int s = 0; s < HV2_N; s++) hv2_step(s); });
  for (int s = 0; s < HV2_N; s++) { char id[40]; snprintf(id, sizeof id, "HV2:only-step-%s", HV2_NAMES[s]); add(id, [s] { init0(); hv2_step(s); }); }

  // ===== low-clock move to the other PLL (#65 third scenario), 2026-10-08 =====
  add("MV:155clk0-99clk2", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:155clk0-99clk2-then-tune-x200", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2);
    for (int i = 0; i < 200; i++) sf((99000000ULL - 20000ULL * i) * M + i, 2); });
  add("MV:both-held-keeps-old", [] { init0(); mssrc(1, 1); sf(130000000 * M, 1); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:dest-shared-reuse", [] { init0(); mssrc(1, 1); sf(14000000 * M, 1); sf(155000000 * M, 0); sf(99000000 * M, 2); sf(14100000 * M, 1); });
  add("MV:dest-shared-low-keeps-old", [] { init0(); mssrc(1, 1); sfm(10000000 * M, 60000000000ULL, 1); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:dest-free-low-replan", [] { init0(); spll(60000000000ULL, 1); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:clk6-on-pllb-then-move", [] { init0(); sf(23000000 * M, 6); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:clk6-even-on-pllb-then-move", [] { init0(); sf(100000000 * M, 6); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:move-then-clk6-first-set", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); sf(23000000 * M, 6); });
  add("MV:move-then-clk7-first-set", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); sf(20000000 * M, 7); });
  add("MV:back-and-forth", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); sf(10000000 * M, 0); sf(98000000 * M, 2);
    sf(160000000 * M, 0); sf(97000000 * M, 2); sf(120000000 * M, 2); sf(99000000 * M, 2); sf(99000000 * M, 0); sf(160000000 * M, 2); sf(99000000 * M, 0); });
  add("MV:pllb-held-move-to-plla", [] { init0(); mssrc(1, 1); mssrc(3, 1); sf(155000000 * M, 1); sf(99000000 * M, 3); });
  add("MV:move-with-divby4-exit", [] { init0(); sf(160000000 * M, 2); reset_(); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  add("MV:manual-not-moved", [] { init0(); sf(155000000 * M, 0); sfm(99000000 * M, 62000000000ULL, 2); });
  add("MV:two-low-clocks-move", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); sf(95000000 * M, 3); sf(14000000 * M, 4); });
  add("MV:rdiv-low-freq-no-move", [] { init0(); sf(155000000 * M, 0); sf(5000 * M, 2); sf(512000 * M, 3); });
  add("MV:corr-then-move", [] { init_(LOAD, 0, 120000); sf(155000000 * M, 0); sf(99000000 * M, 2); });
  // ===== double-reset cases (one reset after all writes) =====
  add("DR:200-120-gt100-exit", [] { init0(); sf(200000000 * M, 0); sf(120000000 * M, 0); });
  add("DR:155-96-replan-exit", [] { init0(); sf(155000000 * M, 0); sf(96000000 * M, 0); });
  add("DR:peer-recalc-exit", [] { init0(); sf(10000000 * M, 1); sf(160000000 * M, 0); sf(120000000 * M, 0); sf(90000000 * M, 0); });
  add("DR:two-divby4-peers-reset", [] { init0(); sf(160000000 * M, 0); reset_(); sf(160000000 * M, 1); sf(120000000 * M, 1); });

  add("HV3:verify-v3-all-steps", [] { init0(); for (int s = 0; s < HV3_N; s++) hv3_step(s); });
  for (int s = 12; s < HV3_N; s++) { char id[40]; snprintf(id, sizeof id, "HV3:only-step-%s", HV3S[s].name); add(id, [s] { init0(); hv3_step(s); }); }

  // ===== v2.3.1 fix sequences (2026-10-10) =====
  // item 1: MS6/7 divider range (DEV-03, DEV-15) + CLK6/7 small bugs
  add("V:dev03-clk6-10kHz", [] { init0(); sf(10000 * M, 6); });
  add("V:dev03-clk7-10kHz", [] { init0(); sf(10000 * M, 7); });
  add("V:dev03-clk6-old-min-18454Hz", [] { init0(); sf(18454 * M, 6); });
  add("V:dev03-clk6-new-min-18455Hz", [] { init0(); sf(18455 * M, 6); });
  add("V:dev03-clk6-band-window-r64", [] { init0(); sf(3690900ULL, 6); });          // 36909.00 Hz -> MS 2.362176 MHz at R=64
  add("V:dev03-clk6-band-window-r1", [] { init0(); sf(236215000ULL, 6); });         // 2.36215 MHz at R=1
  add("V:clk6-sweep-low", [] { for (ull f = 1000; f <= 3000000; f = f * 21 / 20 + 7) { init0(); sf(f * M, 6); } });
  add("V:clk7-sweep-all", [] { for (ull f = 15000; f <= 160000000; f = f * 13 / 10 + 3) { init0(); sf(f * M, 7); } });
  add("V:clk6-150MHz-clamp", [] { init0(); sf(160000000 * M, 6); });
  add("V:dev15-manual-clk7-87p4", [] { init0(); sfm(87400000 * M, 874000000ULL * M, 7); });
  add("V:dev15-manual-clk6-1MHz-needs-R4", [] { init0(); sfm(1000000 * M, 800000000ULL * M, 6); });
  add("V:dev15-manual-clk6-10kHz-no-valid", [] { init0(); sfm(10000 * M, 800000000ULL * M, 6); });
  add("V:dev15-manual-clk6-13MHz-non-integer", [] { init0(); sfm(13000000 * M, 800000000ULL * M, 6); });
  add("V:dev15-manual-clk6-80MHz-odd-11", [] { init0(); sfm(80000000 * M, 880000000ULL * M, 6); });
  add("V:dev15-manual-clk7-3MHz-odd-after-R", [] { init0(); sfm(3000000 * M, 774000000ULL * M, 7); });
  add("V:peer-ok-then-uninit-case", [] { init0(); sf(23000000 * M, 6); sf(100000 * M, 7); sf(87400000 * M, 7); });
  add("V:peer-needs-R16", [] { init0(); sf(23000000 * M, 6); sf(273125 * M, 7); });
  add("V:freq0-clk7-peer-set", [] { init0(); sf(23000000 * M, 6); sf(0, 7); });
  add("V:freq0-clk6-no-peer", [] { init0(); sf(0, 6); });
  add("V:freq0-manual-clk6", [] { init0(); sfm(0, 800000000ULL * M, 6); });
  // item 2: DEV-14 CLK6/7 output enable
  add("V:dev14-clk6-clk7-enable", [] { init0(); sf(23000000 * M, 6); sf(87400000 * M, 7); });
  add("V:dev14-clk6-reject-stays-off", [] { init0(); sf(23000000 * M, 6); sf(12345678 * M, 7); });
  add("V:dev14-clk6-after-user-disable", [] { init0(); sf(23000000 * M, 6); oe(6, 0); sf(24000000 * M, 6); });
  // item 4: 200 MHz clamp
  add("V:fout-200-210-225-250-clk0", [] { init0(); sf(200000000 * M, 0); sf(210000000 * M, 0); sf(225000000 * M, 0); sf(250000000 * M, 0); });
  add("V:fout-manual-210-clk0", [] { init0(); sfm(210000000 * M, 840000000ULL * M, 0); });
  add("V:fout-210-clk1-plus-14-clk0", [] { init0(); sf(14000000 * M, 0); sf(210000000 * M, 1); });
  // item 4: manual MS ratio 5 (stale a); 6.67/7 unchanged on purpose
  add("V:manual-ratio5-140-at-700", [] { init0(); sfm(140000000 * M, 700000000ULL * M, 0); });
  add("V:manual-ratio6p67-unchanged", [] { init0(); sfm(90000000 * M, 600000000ULL * M, 0); });
  add("V:manual-ratio7-unchanged", [] { init0(); sfm(100000000 * M, 700000000ULL * M, 0); });
  // item 4: set_phase / set_int on CLK6/7
  add("V:phase-clk0-5-and-6-7", [] { init0(); for (int c = 0; c < 8; c++) sph(c, 10 + c); });
  add("V:set_int-clk6-clk7", [] { init0(); sf(14000000 * M, 0); sint(6, 1); sint(7, 1); sint(6, 0); sint(0, 1); });
  // item 4: CLKIN_DIV field
  add("V:clkin-div-2-then-4", [] { init0(); reff(50000000, 1); pllin(0, 1); reff(80000000, 1); pllin(1, 1); });
  add("V:clkin-div-4-then-1-then-xo", [] { init0(); reff(80000000, 1); pllin(0, 1); reff(25000000, 1); pllin(1, 1); pllin(1, 0); });
  // item 4: crystal load
  add("V:xtal-load-0pf", [] { init_(SI5351_CRYSTAL_LOAD_0PF, 0, 0); });
  add("V:xtal-load-6-8-10pf", [] { init_(SI5351_CRYSTAL_LOAD_6PF, 0, 0); init_(SI5351_CRYSTAL_LOAD_8PF, 0, 0); init_(SI5351_CRYSTAL_LOAD_10PF, 0, 0); });
  // item 4: VCXO denominator
  add("V:vcxo-then-correction", [] { init0(); svcxo(87500000000ULL, 100); corr_(2000, 0); });
  add("V:vcxo-example-manual-on-pllb", [] { init0(); svcxo(87600000000ULL, 30); mssrc(0, 1); sfm(14600000000ULL, 87600000000ULL, 0); corr_(-500, 0); });
  add("V:vcxo-875-manual-on-pllb-b0", [] { init0(); svcxo(87500000000ULL, 30); mssrc(0, 1); sfm(14583333333ULL, 87500000000ULL, 0); });
  add("V:vcxo-then-pll-input", [] { init0(); svcxo(87500000000ULL, 100); pllin(1, 0); });
  add("V:vcxo-then-reset-normal-pllb", [] { init0(); svcxo(87500000000ULL, 100); reset_(); spll(87500000000ULL, 1); corr_(100, 0); });
  // item 4: DEV-13 5 MHz reference (documented, not changed)
  add("V:dev13-5MHz-ref", [] { init_(LOAD, 5000000, 0); sf(10000000 * M, 0); });

  // ===== v2.3.1 follow-up: recalc clocks when their PLL changes =====
  add("RC:clk1-10M-pllb-then-clk6-plans-pllb", [] { init0(); mssrc(1, 1); sf(10000000 * M, 1); sf(23000000 * M, 6); });
  add("RC:clk6-25M-then-clk0-150M-on-pllb", [] { init0(); sf(25000000 * M, 6); mssrc(0, 1); sf(150000000 * M, 0); });
  add("RC:clk6-23M-then-clk0-150M-on-pllb-left", [] { init0(); sf(23000000 * M, 6); mssrc(0, 1); sf(150000000 * M, 0); });
  add("RC:clk1-10M-then-clk0-150M-plla", [] { init0(); sf(10000000 * M, 1); sf(150000000 * M, 0); });
  add("RC:direct-set_pll-clk0-10M", [] { init0(); sf(10000000 * M, 0); spll(70000000000ULL, 0); });
  add("RC:direct-set_pll-same-freq-no-writes", [] { init0(); sf(10000000 * M, 0); spll(80000000000ULL, 0); });
  add("RC:vcxo-with-clk1-on-pllb", [] { init0(); mssrc(1, 1); sf(10000000 * M, 1); svcxo(87600000000ULL, 30); corr_(500, 0); });
  add("RC:manual-clk2-then-set_pll", [] { init0(); sfm(14000000 * M, 80000000000ULL, 2); spll(72000000000ULL, 0); });
  add("RC:manual-divby4-left-on-set_pll", [] { init0(); sfm(175000000 * M, 70000000000ULL, 0); sf(10000000 * M, 1); spll(80000000000ULL, 0); });
  add("RC:clk7-87p4-then-set_pll-pllb", [] { init0(); sf(23000000 * M, 6); sf(87400000 * M, 7); spll(69000000000ULL, 1); });

  // ===== seeded random fuzz =====
  for (int i = 0; i < 400; i++) {
    char id[32]; snprintf(id, sizeof id, "F:fuzz-%03d", i);
    ull seed = 0x5155351ULL * (i + 1);
    add(id, [seed] { fuzz_seq(seed, 16); });
  }
}

// ---- JSON output ----
static void jstr(FILE *o, const std::string &s) {
  fputc('"', o);
  for (char ch : s) { if (ch == '"' || ch == '\\') fputc('\\', o); fputc(ch, o); }
  fputc('"', o);
}

int main(int argc, char **argv) {
  const char *outpath = argc > 1 ? argv[1] : "out.jsonl";
  FILE *o = fopen(outpath, "w");
  build();
  for (int pattern = 0; pattern < 2; pattern++) {
    for (auto &sc : SC) {
      g_sim.reset(pattern, sc.present);
      RETS.clear();
      memset(objbuf, 0, sizeof objbuf);   // mimic Arduino global object (.bss zero-init)
      S = new (objbuf) Si5351();
      signal(SIGFPE, on_fpe);
      if (sigsetjmp(g_fpe_jb, 1)) { g_sim.mark("CRASH SIGFPE (division by zero)"); RETS.push_back("CRASH=SIGFPE"); }
      else sc.fn();
      fprintf(o, "{\"id\":"); jstr(o, sc.id);
      fprintf(o, ",\"pattern\":%d,\"rets\":[", pattern);
      for (size_t i = 0; i < RETS.size(); i++) { if (i) fputc(',', o); jstr(o, RETS[i]); }
      fprintf(o, "],\"regs\":[");
      for (int i = 0; i < 256; i++) fprintf(o, i ? ",%u" : "%u", g_sim.regs[i]);
      fprintf(o, "],\"state\":{\"plla_freq\":%llu,\"pllb_freq\":%llu,\"clk_freq\":[", (ull)S->plla_freq, (ull)S->pllb_freq);
      for (int i = 0; i < 8; i++) fprintf(o, i ? ",%llu" : "%llu", (ull)S->clk_freq[i]);
      fprintf(o, "],\"pll_assignment\":[");
      for (int i = 0; i < 8; i++) fprintf(o, i ? ",%d" : "%d", (int)S->pll_assignment[i]);
      fprintf(o, "],\"xtal_freq\":[%u,%u],\"ref_osc\":[%d,%d],\"corr\":[%d,%d]}", S->xtal_freq[0], S->xtal_freq[1],
              (int)S->plla_ref_osc, (int)S->pllb_ref_osc, S->get_correction(SI5351_PLL_INPUT_XO), S->get_correction(SI5351_PLL_INPUT_CLKIN));
      fprintf(o, ",\"n_writes\":%lu,\"n_reads\":%lu,\"tx\":[", g_sim.n_writes, g_sim.n_reads);
      for (size_t i = 0; i < g_sim.events.size(); i++) { if (i) fputc(',', o); jstr(o, g_sim.events[i]); }
      fprintf(o, "]}\n");
      S->~Si5351();
    }
  }
  fclose(o);
  fprintf(stderr, "%zu scenarios x 2 register-file patterns written to %s\n", SC.size(), outpath);
  return 0;
}
