CC ?= gcc
THREAD_FLAGS ?= -pthread
CFLAGS ?= -Ofast -march=native -flto=auto -Wall -Wextra -Wpedantic -Iinclude $(THREAD_FLAGS) -g -fno-omit-frame-pointer #(last two for profiling)
RELEASE_FLAGS ?= -Ofast -flto=auto -Iinclude $(THREAD_FLAGS)
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN32_CC ?= i686-w64-mingw32-gcc
BUILD_DIR ?= release

TARGET := $(BUILD_DIR)/Cepimetheus
TUNING_TARGET := $(BUILD_DIR)/engine.so

SRC := \
    src/main.c \
    src/uci.c \
    src/eval.c \
    src/eval_helpers.c \
    src/endgame.c \
    src/search.c \
    src/search_helpers.c \
    src/tt.c \
    src/movepicker.c \
    src/think.c \
    src/movegen.c \
    src/move.c \
    src/board.c \
    src/bitboard.c \
    src/zobrist.c \
    src/see.c

# Source files needed exclusively for evaluation tuning (excludes search/UCI loops)
TUNING_SRC := \
    src/eval.c \
    src/eval_helpers.c \
    src/endgame.c \
    src/movegen.c \
    src/move.c \
    src/board.c \
    src/bitboard.c \
    src/zobrist.c

OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))

.PHONY: all clean run windows tuning release spsa

all: $(TARGET)

spsa:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -DSPSA_TUNING"


$(BUILD_DIR)/%.o: src/%.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ -lm

# Compiles the shared library directly from sources to safely inject -fPIC
tuning: $(TUNING_TARGET)

$(TUNING_TARGET): $(TUNING_SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -shared $^ -o $(TUNING_TARGET)

run: $(TARGET)
	./$(TARGET)

windows:
	$(MAKE) clean
	$(MAKE) CC=$(WIN_CC) TARGET=$(BUILD_DIR)/Cepimetheus.exe THREAD_FLAGS='-pthread -static -static-libgcc'

release:
	mkdir -p $(BUILD_DIR)
	$(CC) $(RELEASE_FLAGS) -march=x86-64 $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-64 -lm
	$(WIN_CC) $(RELEASE_FLAGS) -march=x86-64 -static -static-libgcc $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-64.exe -lm
	$(CC) $(RELEASE_FLAGS) -march=x86-64-v2 $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-POPCNT -lm
	$(WIN_CC) $(RELEASE_FLAGS) -march=x86-64-v2 -static -static-libgcc $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-POPCNT.exe -lm
	$(CC) $(RELEASE_FLAGS) -march=x86-64-v3 -DNO_PEXT $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-AVX2 -lm
	$(WIN_CC) $(RELEASE_FLAGS) -march=x86-64-v3 -DNO_PEXT -static -static-libgcc $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-AVX2.exe -lm
	$(CC) $(RELEASE_FLAGS) -march=x86-64-v3 -DUSE_PEXT $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-BMI2 -lm
	$(WIN_CC) $(RELEASE_FLAGS) -march=x86-64-v3 -DUSE_PEXT -static -static-libgcc $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-BMI2.exe -lm
	$(CC) $(RELEASE_FLAGS) -march=x86-64-v4 -DUSE_PEXT $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-AVX512 -lm
	$(WIN_CC) $(RELEASE_FLAGS) -march=x86-64-v4 -DUSE_PEXT -static -static-libgcc $(SRC) -o $(BUILD_DIR)/Cepimetheus-15.0.0-AVX512.exe -lm

clean:
	rm -rf $(BUILD_DIR)