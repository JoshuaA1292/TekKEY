# Minsky stage 2: the exchange computer over a wire.
CXX ?= clang++
STAGE1 = ../stage1-orderbook-computer/src
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -I$(STAGE1) -Isrc
SAN = -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(STAGE1) -Isrc
REF = $(STAGE1)/engine.cpp $(STAGE1)/minsky.cpp
LIB = src/fastbook.cpp src/transport.cpp src/table.cpp src/router.cpp src/snake.cpp src/programs.cpp
HDRS = src/*.hpp $(STAGE1)/*.hpp

# Bounded keys (Build A): policyc, verify, replay, venued and their tests.
BKLIB = src/program_json.cpp src/policy.cpp src/verifier.cpp src/replayer.cpp src/fastbook.cpp src/transport.cpp
BKHDRS = src/json.hpp src/sha256.hpp src/env.hpp src/program_json.hpp src/policy.hpp src/verifier.hpp src/replayer.hpp \
         src/matcher.hpp src/fastbook.hpp src/proto.hpp src/transport.hpp
ENGINE_SHA := $(shell cat src/fastbook.cpp src/fastbook.hpp | shasum -a 256 | cut -c1-12)
BKFLAGS = -DENGINE_SHA=\"$(ENGINE_SHA)\"
BKBINS = build/policyc build/verify build/replay build/venued build/test_verify build/test_venued

BINS = $(BKBINS) build/exchange build/router build/compile build/bench build/viz build/canary build/test_fastbook build/test_wire build/test_snake build/test_programs build/test_viz build/test_server
all: $(BINS)

build/exchange: src/exchange.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/exchange.cpp $(LIB) $(REF)

build/router: src/router_main.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/router_main.cpp $(LIB) $(REF)

build/test_fastbook: tests/test_fastbook.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_fastbook.cpp $(LIB) $(REF)

build/test_wire: tests/test_wire.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_wire.cpp $(LIB) $(REF)

build/compile: src/compile_main.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/compile_main.cpp $(LIB) $(REF)

build/test_snake: tests/test_snake.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_snake.cpp $(LIB) $(REF)

build/bench: src/bench.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/bench.cpp $(LIB) $(REF)

build/viz: src/viz.cpp src/transport.cpp src/table.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/viz.cpp src/transport.cpp src/table.cpp

build/canary: src/canary.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/canary.cpp $(LIB) $(REF)

build/test_programs: tests/test_programs.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_programs.cpp $(LIB) $(REF)

build/test_viz: tests/test_viz.cpp src/viz.cpp src/transport.cpp src/table.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_viz.cpp src/transport.cpp src/table.cpp

build/test_server: tests/test_server.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_server.cpp $(LIB) $(REF)

build/policyc: src/policyc.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ src/policyc.cpp $(BKLIB)

build/verify: src/verify.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ src/verify.cpp $(BKLIB)

build/replay: src/replay.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ src/replay.cpp $(BKLIB)

build/venued: src/venued.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ src/venued.cpp $(BKLIB)

build/test_verify: tests/test_verify.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ tests/test_verify.cpp $(BKLIB)

build/test_venued: tests/test_venued.cpp $(BKLIB) $(BKHDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(BKFLAGS) -o $@ tests/test_venued.cpp $(BKLIB)

build/test_fastbook-asan: tests/test_fastbook.cpp $(LIB) $(REF) $(HDRS)
	@mkdir -p build
	$(CXX) $(SAN) -o $@ tests/test_fastbook.cpp $(LIB) $(REF)

# Tests that need no running exchange. The wire tests are listed in README.md.
test: build/test_fastbook build/test_programs build/test_snake build/canary
	./build/test_fastbook
	./build/test_programs
	./build/test_snake --quick
	./build/canary --programs 300 --snake-frames 200

# Everything, against an exchange this target starts on ports 7000-7002.
test-all: all
	./build/exchange --tcp 7000 --udp 7001 --shm /tmp/minsky.shm --feed 7002 --quiet & echo $$! > /tmp/minsky-exchange-test.pid; sleep 0.5; \
	./build/test_fastbook && ./build/test_programs && ./build/test_server tcp://127.0.0.1:7000 && \
	./build/test_wire --connect tcp://127.0.0.1:7000 --n 200 && ./build/test_wire --connect udp://127.0.0.1:7001 --n 100 && \
	./build/test_wire --connect shm:///tmp/minsky.shm --n 300 && \
	./build/test_snake --wire tcp://127.0.0.1:7000 && ./build/test_snake --wire shm:///tmp/minsky.shm --keys tcp://127.0.0.1:7000 --quick; \
	status=$$?; kill $$(cat /tmp/minsky-exchange-test.pid); rm -f /tmp/minsky.shm /tmp/minsky-exchange-test.pid; exit $$status

# Bounded keys (Build A): BOUNDED_KEYS.md §8.5 tests 1-5 and 7, determinism, performance, the
# fixtures. Starts its own exchange on 7500-7502 (test_venued starts its own on 7400/7410).
test-bk: build/exchange build/policyc build/verify build/replay build/venued build/test_verify build/test_venued
	./build/exchange --tcp 7500 --feed 7502 --accounts 255 --quiet & echo $$! > /tmp/minsky-bk-test.pid; sleep 0.5; \
	./build/test_verify --wire tcp://127.0.0.1:7500 && ./build/test_venued; \
	status=$$?; kill $$(cat /tmp/minsky-bk-test.pid); rm -f /tmp/minsky-bk-test.pid; exit $$status

# Regenerates every fixture Build A owns (CONTRACT.md §7) from the binaries.
fixtures: build/policyc build/verify build/test_venued build/exchange build/venued
	for v in ex_v1 ex_v2; do ./build/policyc --instruments ../fixtures/instruments.json --env ../fixtures/envelope_40.json \
	  < ../fixtures/$$v.mpl > ../fixtures/$$v.compiled.json; done
	./build/policyc --instruments ../fixtures/instruments.json --env ../fixtures/envelope_40.json \
	  < ../fixtures/ex_noallow.mpl > ../fixtures/ex_noallow.result.json; test $$? -eq 2
	for v in ex_v1 ex_v2; do ./build/verify --instruments ../fixtures/instruments.json --program ../fixtures/$$v.compiled.json \
	  --envelope ../fixtures/envelope_40.json --mark 3010 > ../fixtures/$$v.result.json; done; true
	./build/test_venued --write-fixture > /dev/null
	python3 -I tests/audit_stoplimits.py

clean:
	rm -rf build
.PHONY: all test test-all test-bk fixtures clean
