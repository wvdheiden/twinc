CXX      ?= clang++
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX   ?= /usr/local
ARCHFLAGS ?=
UNAME    := $(shell uname -s)

EXE      :=
WIN      :=
ifeq ($(UNAME),Darwin)
MINOS    := -mmacosx-version-min=11.0
LDLIBS   := -framework Foundation -larchive
MM       := $(wildcard src/*.mm)
else ifneq ($(filter MINGW% MSYS%,$(UNAME)),)
# Windows (MSYS2 UCRT64): one static twinc.exe, UTF-8 code page via the manifest
CXX      := g++
EXE      := .exe
WIN      := 1
MINOS    := -Isrc/win -D_POSIX_THREAD_SAFE_FUNCTIONS -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00
LDLIBS   := -static $(shell pkg-config --static --libs libarchive) -lshell32 -lole32 -luuid
MM       :=
else
MINOS    :=
LDLIBS   := -larchive
MM       :=
endif

ALL_CXXFLAGS := -std=c++17 $(MINOS) $(ARCHFLAGS) $(CXXFLAGS)

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o) $(MM:src/%.mm=build/%.o)
HDR := $(wildcard src/*.h)
ifdef WIN
OBJ += $(patsubst src/win/%.cpp,build/win/%.o,$(wildcard src/win/*.cpp)) build/win/twinc-res.o
HDR += $(wildcard src/win/*.h src/win/sys/*.h)
endif

ifdef WIN
twinc: twinc.exe
.PHONY: twinc
endif

twinc$(EXE): $(OBJ)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.cpp $(HDR) | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/win/%.o: src/win/%.cpp $(HDR) | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/win/twinc-res.o: src/win/twinc.rc src/win/twinc.manifest | build
	windres $< -o $@

build/%.o: src/%.mm $(HDR) | build
	$(CXX) $(ALL_CXXFLAGS) -fobjc-arc -c $< -o $@

build:
	mkdir -p build build/win

# Universal (Apple Silicon + Intel) binary, used for signed releases.
universal:
	$(MAKE) clean
	$(MAKE) ARCHFLAGS="-arch arm64 -arch x86_64"

install: twinc
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/twinc
	install -m 755 twinc $(DESTDIR)$(PREFIX)/bin/twinc
	install -m 644 ncm.sh ncm.zsh $(DESTDIR)$(PREFIX)/share/twinc/

clean:
	rm -rf build twinc twinc.exe

.PHONY: universal install clean
