#ifndef SI5351_SIM_H
#define SI5351_SIM_H
#include <stdint.h>
#include <string>
#include <vector>

struct Si5351Sim {
  uint8_t regs[256];
  uint8_t ptr;              // device register pointer (auto-increments)
  uint8_t dev_addr;         // address the simulated device answers on
  bool present;             // device ACKs?
  // current transmission
  bool in_tx;
  uint8_t tx_addr;
  std::vector<uint8_t> tx_buf;
  // pending read data
  std::vector<uint8_t> rx_buf;
  size_t rx_pos;
  // transcript
  std::vector<std::string> events;
  unsigned long n_writes, n_reads;

  void reset(int pattern, bool present_ = true);
  void mark(const std::string &s) { events.push_back("CALL " + s); }
};
extern Si5351Sim g_sim;
#endif
