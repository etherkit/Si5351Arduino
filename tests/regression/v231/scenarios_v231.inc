// v2.3.1 acceptance + must-not-break call sequences.
// Same wrapper DSL as /workspace/si5351-equiv/harness.cpp (init_/sf/sfm/svcxo/corr_/mssrc/pllin/reff/...),
// so these add(...) lines can be pasted into harness.cpp build() unchanged.
// Frequencies are in library units (Hz * 100) via M.
#define L8 SI5351_CRYSTAL_LOAD_8PF
// ---------------- acceptance tests (expected to FAIL on 1ddd43e) ----------------
add("V231:DEV03-clk6-10kHz",            [] { init0(); sf(10000 * M, 6); });
add("V231:CLK67MIN-clk6-15kHz-clamp",   [] { init0(); sf(15000 * M, 6); });
add("V231:DEV15-manual-clk7-87p4-874",  [] { init0(); sfm(87400000 * M, 874000000 * M, 7); });
add("V231:DEV15-manual-clk6-87p4-874",  [] { init0(); sfm(87400000 * M, 874000000 * M, 6); });
add("V231:DEV14-clk6-23MHz-enable",     [] { init0(); sf(23000000 * M, 6); });
add("V231:DEV14-clk7-87p4-after-clk6",  [] { init0(); sf(23000000 * M, 6); sf(87400000 * M, 7); });
add("V231:DEV14-clk7-first-23MHz",      [] { init0(); sf(23000000 * M, 7); });
add("V231:UNINIT-clk7-1MHz-peer-fail",  [] { init0(); sf(23000000 * M, 6); sf(1000000 * M, 7); });
add("V231:DIV0-clk7-zero-with-peer",    [] { init0(); sf(23000000 * M, 6); sf(0, 7); });
add("V231:DIV0-clk6-zero-with-peer",    [] { init0(); sf(23000000 * M, 7); sf(0, 6); });
add("V231:FMAX-clk0-210MHz",            [] { init0(); sf(210000000 * M, 0); });
add("V231:FMAX-clk0-225MHz",            [] { init0(); sf(225000000 * M, 0); });
add("V231:FMAX-manual-clk0-225MHz-900", [] { init0(); sfm(225000000 * M, 900000000 * M, 0); });
add("V231:MSRATIO-manual-140-700-r5",   [] { init0(); sfm(140000000 * M, 700000000 * M, 0); });
add("V231:MSRATIO-manual-90-600-r6p67", [] { init0(); sfm(90000000 * M, 600000000 * M, 0); });
add("V231:MSRATIO-manual-100-700-r7",   [] { init0(); sfm(100000000 * M, 700000000 * M, 0); });
add("V231:PHASE-clk6-reg171",           [] { init0(); phase_(6, 10); });
add("V231:PHASE-clk7-reg172",           [] { init0(); phase_(7, 10); });
add("V231:SETINT-clk6-fba-int",         [] { init0(); corr_(100000, 0); setint_(6, 1); });
add("V231:SETINT-clk7-fbb-int",         [] { init0(); corr_(100000, 0); setint_(7, 1); });
add("V231:CLKINDIV-50-then-80MHz",      [] { init0(); reff(50000000, 1); pllin(0, 1); reff(80000000, 1); pllin(0, 1); });
add("V231:XTAL-0PF-reserved",           [] { init_(SI5351_CRYSTAL_LOAD_0PF, 0, 0); });
add("V231:VCXO-c-after-set_correction", [] { init0(); svcxo(876000000 * M, 40); corr_(0, 0); });
add("V231:VCXO-c-after-set_pll_input",  [] { init0(); svcxo(876000000 * M, 40); pllin(0, 0); });
add("V231:DEV13-ref-5MHz",              [] { init_(L8, 5000000, 0); });
// ---------------- must-not-break (pass on 1ddd43e; values locked from golden_cases.json / 1ddd43e) ----------------
add("MNB:clk0-10MHz",                   [] { init0(); sf(10000000 * M, 0); });
add("MNB:clk0-14p07MHz",                [] { init0(); sf(14070000 * M, 0); });
add("MNB:clk0-100MHz",                  [] { init0(); sf(100000000 * M, 0); });
add("MNB:clk0-112p5MHz",                [] { init0(); sf(112500000 * M, 0); });
add("MNB:clk0-150MHz-divby4",           [] { init0(); sf(150000000 * M, 0); });
add("MNB:clk0-200MHz-divby4",           [] { init0(); sf(200000000 * M, 0); });
add("MNB:clk3-200MHz-divby4",           [] { init0(); sf(200000000 * M, 3); });
add("MNB:clk0-4kHz",                    [] { init0(); sf(4000 * M, 0); });
add("MNB:clk2-5kHz",                    [] { init0(); sf(5000 * M, 2); });
add("MNB:clk0-512kHz",                  [] { init0(); sf(512000 * M, 0); });
add("MNB:clk6-23MHz",                   [] { init0(); sf(23000000 * M, 6); });
add("MNB:clk7-87p4-after-clk6",         [] { init0(); sf(23000000 * M, 6); sf(87400000 * M, 7); });
add("MNB:clk7-reject-12p345678",        [] { init0(); sf(23000000 * M, 6); sf(12345678 * M, 7); });
add("MNB:clk7-reject-odd-ratio",        [] { init0(); sf(23000000 * M, 6); sf(874000000 / 19 * M, 7); });
add("MNB:clk6-100kHz-R6",               [] { init0(); sf(100000 * M, 6); });
add("MNB:clk7-50kHz-R7",                [] { init0(); sf(50000 * M, 7); });
add("MNB:clk6-150MHz-clamp",            [] { init0(); sf(150000000 * M, 6); });
add("MNB:manual-clk1-175-700-divby4",   [] { init0(); mssrc(1, 1); sfm(175000000 * M, 700000000 * M, 1); });
add("MNB:manual-clk0-14-800-parity",    [] { init0(); sfm(14000000 * M, 800000000 * M, 0); });
add("MNB:clk1-pllb-14MHz",              [] { init0(); mssrc(1, 1); sf(14000000 * M, 1); });
add("MNB:vcxo-876-40ppm",               [] { init0(); svcxo(876000000 * M, 40); });
add("MNB:pll-assign-after-reset",       [] { init0(); });
add("MNB:clk7-pll-assignment-lock",     [] { init0(); sf(87400000 * M, 7); });
add("MNB:set_ms-byte2-rmw-5kHz",        [] { init0(); wr_(44, 0x80); sf(5000 * M, 0); });
add("MNB:divby4-exit-reset-155-96",     [] { init0(); sf(155000000 * M, 0); sf(96000000 * M, 0); });
add("MNB:divby4-exit-reset-156-80",     [] { init0(); sf(80000000 * M, 0); sf(156000000 * M, 0); sf(80000000 * M, 0); });
add("MNB:low-clock-moves-155-clk0-99-clk2", [] { init0(); sf(155000000 * M, 0); sf(99000000 * M, 2); });
add("MNB:both-plls-held-return-code",   [] { init0(); sf(155000000 * M, 0); mssrc(1, 1); sf(160000000 * M, 1); sf(99000000 * M, 2); });
add("MNB:phase-clk0-mask",              [] { init0(); phase_(0, 0xFF); });
add("MNB:phase-clk5",                   [] { init0(); phase_(5, 0x2A); });
add("MNB:setint-clk0",                  [] { init0(); setint_(0, 1); });
add("MNB:xtal-6pF",                     [] { init_(SI5351_CRYSTAL_LOAD_6PF, 0, 0); });
add("MNB:xtal-8pF",                     [] { init_(SI5351_CRYSTAL_LOAD_8PF, 0, 0); });
add("MNB:xtal-10pF",                    [] { init_(SI5351_CRYSTAL_LOAD_10PF, 0, 0); });
add("MNB:clkin-div4-clean",             [] { init0(); reff(80000000, 1); pllin(0, 1); });
add("MNB:ref-27MHz",                    [] { init_(L8, 27000000, 0); });
add("MNB:corr-100000-14MHz",            [] { init_(L8, 0, 100000); sf(14000000 * M, 0); });
add("MNB:clk0-0Hz",                     [] { init0(); sf(0, 0); });
