CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc
LDFLAGS = -static -static-libgcc -static-libstdc++ -lwinpthread

SRCS = src/util.cpp src/platform.cpp src/skiplist.cpp src/bloom.cpp src/wal.cpp src/sstable.cpp src/db.cpp
OBJS = $(patsubst src/%.cpp,build/obj/%.o,$(SRCS))

all: build/libminidb.a build/test_persist.exe build/test_bench.exe build/test_concurrent.exe

build/obj/%.o: src/%.cpp
	@if not exist build\obj mkdir build\obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libminidb.a: $(OBJS)
	ar rcs $@ $(OBJS)

build/test_persist.exe: tests/test_persist.cpp build/libminidb.a
	$(CXX) $(CXXFLAGS) $< build/libminidb.a -o $@ $(LDFLAGS)

build/test_bench.exe: tests/test_bench.cpp build/libminidb.a
	$(CXX) $(CXXFLAGS) $< build/libminidb.a -o $@ $(LDFLAGS)

build/test_concurrent.exe: tests/test_concurrent.cpp build/libminidb.a
	$(CXX) $(CXXFLAGS) $< build/libminidb.a -o $@ $(LDFLAGS)

clean:
	@if exist build rmdir /s /q build

.PHONY: all clean
