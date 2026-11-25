CXX      ?= clang++
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX   ?= /usr/local
ARCHFLAGS ?=
MINOS    := -mmacosx-version-min=11.0

ALL_CXXFLAGS := -std=c++17 $(MINOS) $(ARCHFLAGS) $(CXXFLAGS)
LDLIBS   := -framework Foundation -larchive

SRC := $(wildcard src/*.cpp)
MM  := $(wildcard src/*.mm)
OBJ := $(SRC:src/%.cpp=build/%.o) $(MM:src/%.mm=build/%.o)
HDR := $(wildcard src/*.h)

twinc: $(OBJ)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.cpp $(HDR) | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/%.o: src/%.mm $(HDR) | build
	$(CXX) $(ALL_CXXFLAGS) -fobjc-arc -c $< -o $@

build:
	mkdir -p build

# Universal (Apple Silicon + Intel) binary, used for signed releases.
universal:
	$(MAKE) clean
	$(MAKE) ARCHFLAGS="-arch arm64 -arch x86_64"

install: twinc
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/twinc
	install -m 755 twinc $(DESTDIR)$(PREFIX)/bin/twinc
	install -m 644 ncm.zsh $(DESTDIR)$(PREFIX)/share/twinc/ncm.zsh

clean:
	rm -rf build twinc

.PHONY: universal install clean
