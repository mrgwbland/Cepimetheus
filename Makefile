CC ?= gcc
THREAD_FLAGS ?= -pthread
CFLAGS ?= -Ofast -march=x86-64-v3 -flto -Wall -Wextra -Wpedantic -Iinclude $(THREAD_FLAGS) -g -fno-omit-frame-pointer #(last two for profiling)
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN32_CC ?= i686-w64-mingw32-gcc

TARGET := Cepimetheus
TUNING_TARGET := engine.so

SRC := \
    src/main.c \
    src/uci.c \
    src/eval.c \
    src/search.c \
    src/think.c \
    src/movegen.c \
    src/move.c \
    src/board.c \
    src/bitboard.c

# Source files needed exclusively for evaluation tuning (excludes search/UCI loops)
TUNING_SRC := \
    src/eval.c \
    src/movegen.c \
    src/move.c \
    src/board.c \
    src/bitboard.c

OBJ := $(SRC:.c=.o)

.PHONY: all clean run windows tuning

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

# Compiles the shared library directly from sources to safely inject -fPIC
tuning: $(TUNING_SRC)
	$(CC) $(CFLAGS) -fPIC -shared $^ -o $(TUNING_TARGET)

run: $(TARGET)
	./$(TARGET)

windows:
	$(MAKE) clean
	$(MAKE) CC=$(WIN_CC) TARGET=Cepimetheus.exe THREAD_FLAGS='-pthread -static -static-libgcc'

clean:
	rm -f $(OBJ) $(TARGET) Cepimetheus.exe $(TUNING_TARGET)