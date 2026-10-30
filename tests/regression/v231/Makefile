# v2.3.1 acceptance runner, built against PINNED master 1ddd43e source (not the live working tree, which other agents are editing) and v2.2.0 (0559cbf).
# pinned: git show 1ddd43e:src/si5351.{cpp,h} (see master_1ddd43e/SHA256SUMS)
MASTER_SRC ?= master_1ddd43e
EQUIV ?= ..
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wno-unused-variable -Wno-unused-result -ftrivial-auto-var-init=pattern
all: build/acc_master build/acc_v220
build/acc_master: acceptance.cpp scenarios_v231.inc $(MASTER_SRC)/si5351.cpp $(MASTER_SRC)/si5351.h
	mkdir -p build logs
	g++ $(CXXFLAGS) -I. -I$(EQUIV)/stubs -I$(MASTER_SRC) acceptance.cpp $(EQUIV)/stubs/Wire.cpp $(MASTER_SRC)/si5351.cpp -o $@ 2> logs/build_acc_master.log || (cat logs/build_acc_master.log; false)
build/acc_v220: acceptance.cpp scenarios_v231.inc
	mkdir -p build logs
	g++ $(CXXFLAGS) -I. -I$(EQUIV)/stubs -I$(EQUIV)/orig acceptance.cpp $(EQUIV)/stubs/Wire.cpp $(EQUIV)/orig/si5351.cpp -o $@ 2> logs/build_acc_v220.log || (cat logs/build_acc_v220.log; false)
run: all
	./build/acc_master logs/acc_master.jsonl
	./build/acc_v220 logs/acc_v220.jsonl
	python3 test_v231.py logs/acc_master.jsonl
