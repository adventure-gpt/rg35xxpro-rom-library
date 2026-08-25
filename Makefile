CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags sdl2 freetype2 json-glib-1.0 libcurl)
LDLIBS := $(shell pkg-config --libs sdl2 freetype2 json-glib-1.0 libcurl) -pthread

.PHONY: all clean test
all: build/rom-library

build/rom-library: src/main.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< $(LDLIBS) -o $@

test: build/rom-library
	python3 tests/helper_tests.py
	ROM_LIBRARY_HOME=/tmp/rom-library-test ROM_LIBRARY_ROMS_ROOT=/tmp/rom-library-test/Roms ROM_LIBRARY_HELPER=$(CURDIR)/scripts/romlib_helper.py ./build/rom-library --self-test

clean:
	rm -f build/rom-library
