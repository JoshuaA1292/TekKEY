CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
SAN = -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SRC = src/engine.cpp src/minsky.cpp

all: build/tests build/experiments build/stress build/rule110

build/tests: src/tests.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/tests.cpp $(SRC)

build/experiments: src/experiments.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/experiments.cpp $(SRC)

build/stress: src/stress.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/stress.cpp $(SRC)

build/stress-asan: src/stress.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(SAN) -o $@ src/stress.cpp $(SRC)

build/rule110: src/rule110.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/rule110.cpp $(SRC)

rule110: build/rule110
	./build/rule110 48 24

stress: build/stress
	./build/stress | tee stress.txt

build/tests-asan: src/tests.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(SAN) -o $@ src/tests.cpp $(SRC)

test: build/tests build/tests-asan
	./build/tests && ./build/tests-asan

run: all
	./build/tests && ./build/experiments | tee results.txt

clean:
	rm -rf build
.PHONY: all test run clean

build/experiments-asan: src/experiments.cpp $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(SAN) -o $@ src/experiments.cpp $(SRC)

sanitize: build/tests-asan build/experiments-asan build/stress-asan
	./build/tests-asan && ./build/experiments-asan --quick && ./build/stress-asan --quick
.PHONY: sanitize stress rule110
