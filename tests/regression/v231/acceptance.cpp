// v2.3.1 acceptance runner. Same stubs, wrapper DSL and JSONL record format as
// /workspace/si5351-equiv/harness.cpp; adds per-scenario fork() so a crash (SIGFPE in
// set_freq(0) on CLK6/7) is recorded as {"crash":"SIGFPE"} instead of killing the run.
#include "si5351.h"
#include "sim.h"
#include <new>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <functional>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
typedef unsigned long long ull;
static const ull M = SI5351_FREQ_MULT;
alignas(Si5351) static unsigned char objbuf[sizeof(Si5351)];
static Si5351 *S;
static std::vector<std::string> RETS;
static void mark(const char *fmt, ...) { char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_sim.mark(b); }
static void ret(const char *n, long long v) { char b[96]; snprintf(b, sizeof b, "%s=%lld", n, v); RETS.push_back(b); g_sim.events.push_back(std::string("RET ") + b); }
static bool init_(uint8_t load, uint32_t xo, int32_t corr) { mark("init(%u,%u,%d)", load, xo, corr); bool r = S->init(load, xo, corr); ret("init", r); return r; }
static uint8_t sf(ull f, int clk) { mark("set_freq(%llu,CLK%d)", f, clk); uint8_t r = S->set_freq(f, (enum si5351_clock)clk); ret("set_freq", r); return r; }
static uint8_t sfm(ull f, ull p, int clk) { mark("set_freq_manual(%llu,%llu,CLK%d)", f, p, clk); uint8_t r = S->set_freq_manual(f, p, (enum si5351_clock)clk); ret("set_freq_manual", r); return r; }
static void svcxo(ull p, int ppm) { mark("set_vcxo(%llu,%d)", p, ppm); S->set_vcxo(p, ppm); }
static void corr_(int32_t c, int ref) { mark("set_correction(%d,%d)", c, ref); S->set_correction(c, (enum si5351_pll_input)ref); }
static void mssrc(int clk, int pll) { mark("set_ms_source(CLK%d,PLL%c)", clk, pll ? 'B' : 'A'); S->set_ms_source((enum si5351_clock)clk, (enum si5351_pll)pll); }
static void pllin(int pll, int in) { mark("set_pll_input(%d,%d)", pll, in); S->set_pll_input((enum si5351_pll)pll, (enum si5351_pll_input)in); }
static void reff(uint32_t f, int ref) { mark("set_ref_freq(%u,%d)", f, ref); S->set_ref_freq(f, (enum si5351_pll_input)ref); }
static void phase_(int clk, int ph) { mark("set_phase(CLK%d,%d)", clk, ph); S->set_phase((enum si5351_clock)clk, (uint8_t)ph); }
static void setint_(int clk, int en) { mark("set_int(CLK%d,%d)", clk, en); S->set_int((enum si5351_clock)clk, (uint8_t)en); }
static void wr_(int reg, int v) { mark("si5351_write(%d,%d)", reg, v); S->si5351_write((uint8_t)reg, (uint8_t)v); }
static void init0() { init_(SI5351_CRYSTAL_LOAD_8PF, 0, 0); }
struct Scn { std::string id; std::function<void()> fn; };
static std::vector<Scn> SC;
static void add(const std::string &id, std::function<void()> fn) { SC.push_back({id, fn}); }
static void jstr(FILE *o, const std::string &s) { fputc('"', o); for (char c : s) { if (c == '"' || c == '\\') fputc('\\', o); fputc(c, o); } fputc('"', o); }
static void build() {
#include "scenarios_v231.inc"
}
int main(int argc, char **argv) {
  const char *outpath = argc > 1 ? argv[1] : "v231.jsonl";
  FILE *o = fopen(outpath, "w");
  build();
  for (int pattern = 0; pattern < 2; pattern++)
    for (auto &sc : SC) {
      fflush(o);
      int pfd[2]; pipe(pfd);
      pid_t pid = fork();
      if (pid == 0) {
        close(pfd[0]); FILE *w = fdopen(pfd[1], "w");
        g_sim.reset(pattern, true); RETS.clear();
        memset(objbuf, 0, sizeof objbuf); S = new (objbuf) Si5351();
        sc.fn();
        fprintf(w, "{\"id\":"); jstr(w, sc.id);
        fprintf(w, ",\"pattern\":%d,\"crash\":null,\"rets\":[", pattern);
        for (size_t i = 0; i < RETS.size(); i++) { if (i) fputc(',', w); jstr(w, RETS[i]); }
        fprintf(w, "],\"regs\":[");
        for (int i = 0; i < 256; i++) fprintf(w, i ? ",%u" : "%u", g_sim.regs[i]);
        fprintf(w, "],\"state\":{\"plla_freq\":%llu,\"pllb_freq\":%llu,\"clk_freq\":[", (ull)S->plla_freq, (ull)S->pllb_freq);
        for (int i = 0; i < 8; i++) fprintf(w, i ? ",%llu" : "%llu", (ull)S->clk_freq[i]);
        fprintf(w, "],\"pll_assignment\":[");
        for (int i = 0; i < 8; i++) fprintf(w, i ? ",%d" : "%d", (int)S->pll_assignment[i]);
        fprintf(w, "],\"xtal_freq\":[%u,%u]},\"tx\":[", S->xtal_freq[0], S->xtal_freq[1]);
        for (size_t i = 0; i < g_sim.events.size(); i++) { if (i) fputc(',', w); jstr(w, g_sim.events[i]); }
        fprintf(w, "]}\n"); fclose(w); _exit(0);
      }
      close(pfd[1]);
      std::string buf; char b[65536]; ssize_t n;
      while ((n = read(pfd[0], b, sizeof b)) > 0) buf.append(b, n);
      close(pfd[0]);
      int st; waitpid(pid, &st, 0);
      if (WIFSIGNALED(st)) {
        fprintf(o, "{\"id\":"); jstr(o, sc.id);
        fprintf(o, ",\"pattern\":%d,\"crash\":\"%s\",\"rets\":[],\"regs\":null,\"state\":null,\"tx\":[]}\n", pattern, strsignal(WTERMSIG(st)));
      } else fputs(buf.c_str(), o);
    }
  fclose(o);
  fprintf(stderr, "%zu scenarios x 2 patterns -> %s\n", SC.size(), outpath);
}
