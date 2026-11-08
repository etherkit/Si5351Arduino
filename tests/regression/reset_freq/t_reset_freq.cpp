// reset_freq() regression:
//   1. reset_freq() then set_freq() re-enables that output, like a first call
//   2. an invalid clock index is a no-op
//   3. every other clock is left unchanged
// Compiled with -Dprivate=public so clk_first_set can be checked directly.
#include "si5351.h"
#include "sim.h"
#include <new>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(c, msg) do { \
  bool _c = (c); \
  printf("%s  %s\n", _c ? "PASS" : "FAIL", msg); \
  if (!_c) fails++; \
} while (0)

alignas(Si5351) static unsigned char objbuf[sizeof(Si5351)];
static Si5351 *S;

static const uint64_t F_10M = 1000000000ULL;
static const uint64_t F_14M = 1400000000ULL;
static const uint64_t F_5M  = 500000000ULL;
static const uint64_t F_20M = 2000000000ULL;
static const uint64_t F_23M = 2300000000ULL;

struct State {
  uint8_t regs[256];
  uint64_t clk_freq[8];
  bool first[8];
  enum si5351_pll pll_assignment[8];
  uint64_t plla, pllb;
  uint8_t vcxo;
  uint8_t i2c_error;
  unsigned long n_writes, n_reads;
};

static bool enabled(int clk)
{
  return (g_sim.regs[SI5351_OUTPUT_ENABLE_CTRL] & (1u << clk)) == 0;
}

static void take(State *st)
{
  memcpy(st->regs, g_sim.regs, sizeof st->regs);
  for (int i = 0; i < 8; i++) {
    st->clk_freq[i] = S->clk_freq[i];
    st->first[i] = S->clk_first_set[i];
    st->pll_assignment[i] = S->pll_assignment[i];
  }
  st->plla = S->plla_freq;
  st->pllb = S->pllb_freq;
  st->vcxo = S->vcxo_pull;
  st->i2c_error = S->i2c_error;
  st->n_writes = g_sim.n_writes;
  st->n_reads = g_sim.n_reads;
}

static bool same_members(const State *b)
{
  if (S->plla_freq != b->plla || S->pllb_freq != b->pllb) return false;
  if (S->vcxo_pull != b->vcxo || S->i2c_error != b->i2c_error) return false;
  for (int i = 0; i < 8; i++) {
    if (S->clk_freq[i] != b->clk_freq[i]) return false;
    if (S->clk_first_set[i] != b->first[i]) return false;
    if (S->pll_assignment[i] != b->pll_assignment[i]) return false;
  }
  return true;
}

// True when nothing except clk's output-enable bit changed.
static bool only_oe_bit(const State *b, int clk)
{
  for (int r = 0; r < 256; r++) {
    uint8_t before = b->regs[r];
    uint8_t after = g_sim.regs[r];
    if (r == SI5351_OUTPUT_ENABLE_CTRL) {
      uint8_t mask = (uint8_t)~(1u << clk);
      if ((before & mask) != (after & mask)) return false;
    } else if (before != after) {
      return false;
    }
  }
  if (S->plla_freq != b->plla || S->pllb_freq != b->pllb) return false;
  if (S->vcxo_pull != b->vcxo || S->i2c_error != b->i2c_error) return false;
  for (int i = 0; i < 8; i++) {
    if (S->pll_assignment[i] != b->pll_assignment[i]) return false;
    if (i == clk) continue;
    if (S->clk_freq[i] != b->clk_freq[i]) return false;
    if (S->clk_first_set[i] != b->first[i]) return false;
  }
  return true;
}

static bool fresh(const char *tag)
{
  g_sim.reset(0, true);
  memset(objbuf, 0xA5, sizeof objbuf);
  S = new (objbuf) Si5351();
  bool ok = S->init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  char buf[96];
  snprintf(buf, sizeof buf, "%s init()", tag);
  CHECK(ok, buf);
  bool clocks_idle = g_sim.regs[SI5351_OUTPUT_ENABLE_CTRL] == 0xFF;
  for (int i = 0; i < 8; i++) {
    if (S->clk_freq[i] != 0 || S->clk_first_set[i]) clocks_idle = false;
  }
  snprintf(buf, sizeof buf, "%s init leaves every output at 0 Hz and disabled", tag);
  CHECK(clocks_idle, buf);
  return ok && clocks_idle;
}

// Set clk (enabled) and other. reset_freq(clk) must disable clk and leave
// other alone; the next set_freq(clk) must enable clk again. A later manual
// disable must not arm another auto-enable until reset_freq() is called.
static void reenable_case(const char *tag, int clk, uint64_t freq, int other, uint64_t other_freq)
{
  char buf[160];
  if (!fresh(tag)) return;

  uint8_t r_other = S->set_freq(other_freq, (enum si5351_clock)other);
  uint8_t r_clk = S->set_freq(freq, (enum si5351_clock)clk);
  snprintf(buf, sizeof buf, "%s set_freq CLK%d and CLK%d returns 0", tag, other, clk);
  CHECK(r_other == 0 && r_clk == 0, buf);
  snprintf(buf, sizeof buf, "%s both outputs enabled after first set_freq", tag);
  CHECK(enabled(clk) && enabled(other) && S->clk_first_set[clk] && S->clk_first_set[other], buf);

  State before;
  take(&before);
  S->reset_freq((enum si5351_clock)clk);

  snprintf(buf, sizeof buf, "%s reset_freq zeros CLK%d, disables it, clears first-set", tag, clk);
  CHECK(S->clk_freq[clk] == 0 && !enabled(clk) && S->clk_first_set[clk] == false, buf);
  snprintf(buf, sizeof buf, "%s reset_freq touches only CLK%d's output-enable bit", tag, clk);
  CHECK(only_oe_bit(&before, clk), buf);
  snprintf(buf, sizeof buf, "%s reset_freq is one output-enable read-modify-write", tag);
  CHECK(g_sim.n_writes == before.n_writes + 1 && g_sim.n_reads == before.n_reads + 1, buf);
  snprintf(buf, sizeof buf, "%s CLK%d unchanged by reset_freq", tag, other);
  CHECK(S->clk_freq[other] == other_freq && enabled(other) && S->clk_first_set[other], buf);

  uint8_t r_on = S->set_freq(freq, (enum si5351_clock)clk);
  snprintf(buf, sizeof buf, "%s set_freq after reset_freq re-enables CLK%d", tag, clk);
  CHECK(r_on == 0 && enabled(clk) && S->clk_first_set[clk] && S->clk_freq[clk] == freq, buf);
  snprintf(buf, sizeof buf, "%s CLK%d frequency, enable, and first-set unchanged", tag, other);
  CHECK(S->clk_freq[other] == other_freq && enabled(other) && S->clk_first_set[other], buf);
  snprintf(buf, sizeof buf, "%s PLL assignment unchanged", tag);
  CHECK(S->pll_assignment[clk] == before.pll_assignment[clk]
        && S->pll_assignment[other] == before.pll_assignment[other], buf);

  // The re-enable consumes the first-set flag, like the original first call.
  S->output_enable((enum si5351_clock)clk, 0);
  uint8_t r_stay = S->set_freq(freq, (enum si5351_clock)clk);
  snprintf(buf, sizeof buf, "%s set_freq after output_enable(0) stays disabled", tag);
  CHECK(r_stay == 0 && !enabled(clk) && S->clk_first_set[clk] && S->clk_freq[clk] == freq, buf);

  S->reset_freq((enum si5351_clock)clk);
  uint8_t r_again = S->set_freq(freq, (enum si5351_clock)clk);
  snprintf(buf, sizeof buf, "%s reset_freq arms auto-enable again", tag);
  CHECK(r_again == 0 && enabled(clk) && S->clk_first_set[clk] && S->clk_freq[clk] == freq, buf);
  snprintf(buf, sizeof buf, "%s CLK%d still enabled", tag, other);
  CHECK(enabled(other) && S->clk_freq[other] == other_freq && S->clk_first_set[other], buf);
}

int main()
{
  // CLK0-5, CLK6, and CLK7 each take a different set_freq path.
  reenable_case("CLK0", 0, F_10M, 1, F_14M);
  reenable_case("CLK3", 3, F_5M, 4, F_20M);
  reenable_case("CLK5", 5, F_20M, 0, F_14M);
  reenable_case("CLK6", 6, F_23M, 0, F_14M);
  reenable_case("CLK7", 7, F_10M, 0, F_14M);

  // Several other clocks at once, including one that was never programmed.
  if (fresh("multi")) {
    CHECK(S->set_freq(F_10M, SI5351_CLK0) == 0, "multi set CLK0");
    CHECK(S->set_freq(F_14M, SI5351_CLK1) == 0, "multi set CLK1");
    CHECK(S->set_freq(F_23M, SI5351_CLK6) == 0, "multi set CLK6");
    State before;
    take(&before);
    S->reset_freq(SI5351_CLK1);
    CHECK(S->clk_freq[1] == 0 && !enabled(1) && !S->clk_first_set[1],
          "multi reset_freq(CLK1) clears only CLK1");
    CHECK(only_oe_bit(&before, 1), "multi other registers and clocks untouched");
    CHECK(S->clk_freq[0] == F_10M && enabled(0) && S->clk_first_set[0], "multi CLK0 untouched");
    CHECK(S->clk_freq[6] == F_23M && enabled(6) && S->clk_first_set[6], "multi CLK6 untouched");
    CHECK(S->clk_freq[2] == 0 && !enabled(2) && !S->clk_first_set[2], "multi idle CLK2 untouched");
    CHECK(S->set_freq(F_14M, SI5351_CLK1) == 0 && enabled(1) && S->clk_freq[1] == F_14M,
          "multi set_freq after reset_freq re-enables CLK1");
    CHECK(S->clk_freq[0] == F_10M && enabled(0) && S->clk_first_set[0],
          "multi CLK0 still untouched after CLK1 set_freq");
    CHECK(S->clk_freq[6] == F_23M && enabled(6) && S->clk_first_set[6],
          "multi CLK6 still untouched after CLK1 set_freq");

    // reset_freq on a clock that was never set must not disturb the others.
    take(&before);
    S->reset_freq(SI5351_CLK2);
    CHECK(S->clk_freq[2] == 0 && !enabled(2) && !S->clk_first_set[2],
          "multi reset_freq(idle CLK2) leaves CLK2 idle");
    CHECK(only_oe_bit(&before, 2), "multi idle reset touches only CLK2's enable bit");
    CHECK(enabled(0) && enabled(1) && enabled(6), "multi active clocks stay enabled");
  }

  // Invalid indices: adjacent members (8, 9) and far indices must not change anything.
  if (fresh("badidx")) {
    CHECK(S->set_freq(F_10M, SI5351_CLK0) == 0, "badidx set CLK0");
    CHECK(S->set_freq(F_14M, SI5351_CLK1) == 0, "badidx set CLK1");
    CHECK(S->set_freq(F_23M, SI5351_CLK6) == 0, "badidx set CLK6");
    const int bad[] = {8, 9, 16, 255, -1};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      State before;
      take(&before);
      S->reset_freq((enum si5351_clock)bad[i]);
      char buf[80];
      snprintf(buf, sizeof buf, "badidx reset_freq(%d) is a no-op", bad[i]);
      CHECK(same_members(&before) && memcmp(g_sim.regs, before.regs, 256) == 0
            && g_sim.n_writes == before.n_writes && g_sim.n_reads == before.n_reads, buf);
    }
    // The object is still usable, so the no-op did not corrupt it.
    CHECK(S->set_freq(F_5M, SI5351_CLK0) == 0 && enabled(0) && S->clk_freq[0] == F_5M,
          "badidx CLK0 still accepts set_freq");
    CHECK(S->clk_freq[1] == F_14M && enabled(1) && S->clk_freq[6] == F_23M && enabled(6),
          "badidx other clocks still intact");
    S->reset_freq(SI5351_CLK0);
    CHECK(S->set_freq(F_10M, SI5351_CLK0) == 0 && enabled(0),
          "badidx reset_freq(CLK0) still re-enables after the invalid calls");
    CHECK(S->clk_freq[1] == F_14M && enabled(1), "badidx CLK1 untouched by the valid reset");
  }

  printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails != 0;
}
