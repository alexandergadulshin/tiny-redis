CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
LDFLAGS :=

SRC := src/server.c src/resp.c src/store.c src/commands.c src/wal.c
HDR := src/resp.h src/store.h src/commands.h src/wal.h
BIN := tiny-redis

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM

run: $(BIN)
	./$(BIN)

.PHONY: all clean run
