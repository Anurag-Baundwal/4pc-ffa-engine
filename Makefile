# 4PC FFA engine. Build with MSYS2 UCRT64 g++ (Windows) or g++/clang++ elsewhere:
#   make            optimized build -> ffa_engine(.exe)
#   make EXE=name   optimized build -> name(.exe)   (OpenBench passes the output name this way)
#   make debug      assertions on, -O1 -g -> ffa_engine_debug(.exe)

CXX      ?= g++
UNAME_S  := $(shell uname -s)
UNAME_M  := $(shell uname -m)

# x86: tune for the build machine (PEXT is used only where the CPU has BMI2). ARM (Android/Termux, ARM Linux,
# Apple Silicon): plain ARMv8, which every 64-bit ARM CPU runs; the code has no ARM-specific paths to enable.
ifneq (,$(filter aarch64 arm64,$(UNAME_M)))
    ARCH ?= -march=armv8-a
else
    ARCH ?= -march=native
endif

# -Wno-class-memaccess is GCC-only (clang warns about unknown warning options); macOS's g++ is clang
IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo 1)
ifneq ($(IS_CLANG),1)
    WNOGCC = -Wno-class-memaccess
endif

CXXFLAGS  = -std=c++17 -O3 $(ARCH) -DUSE_PEXT -DNDEBUG -Wall -Wextra $(WNOGCC) -MMD -MP $(EXTRACXXFLAGS)

ifneq (,$(findstring _NT,$(UNAME_S)))
    # Windows (MSYS2/MinGW): link statically so the exe doesn't pick up another toolchain's libstdc++ DLL.
    # Every thread gets the 16 MB stack reserved here.
    SUFFIX   = .exe
    LDSYS    = -static -Wl,--stack,16777216
else ifeq ($(UNAME_S),Darwin)
    # macOS: use the SDK Xcode selects; the compiler's implicit SDK may be newer than the linker can read
    # (stockfish_4pc 0d9c529, "tapi error: malformed file" on a worker).
    SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
    export SDKROOT
    LDSYS    = -pthread
else
    # Linux, Android (Termux)
    LDSYS    = -pthread
endif

# Output name; on Windows ".exe" is added unless it is already there
EXE    ?= ffa_engine
TARGET  = $(EXE:$(SUFFIX)=)$(SUFFIX)
LDFLAGS   = -O3 $(ARCH) $(LDSYS) $(EXTRALDFLAGS)

SRCS = src/bitboard.cpp src/position.cpp src/pgn4.cpp src/eval.cpp src/search.cpp src/tt.cpp src/uci.cpp src/audit.cpp src/main.cpp
OBJS = $(SRCS:src/%.cpp=build/%.o)
DOBJS = $(SRCS:src/%.cpp=build/debug/%.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

debug: ffa_engine_debug$(SUFFIX)

ffa_engine_debug$(SUFFIX): $(DOBJS)
	$(CXX) $(DOBJS) -o $@ -g $(LDSYS)

build/debug/%.o: src/%.cpp | build/debug
	$(CXX) -std=c++17 -O1 -g $(ARCH) -DUSE_PEXT -Wall -Wextra $(WNOGCC) -MMD -MP -c $< -o $@

build:
	mkdir -p build

build/debug:
	mkdir -p build/debug

clean:
	rm -rf build $(TARGET) ffa_engine_debug$(SUFFIX)

.PHONY: all debug clean

-include $(OBJS:.o=.d) $(DOBJS:.o=.d)
