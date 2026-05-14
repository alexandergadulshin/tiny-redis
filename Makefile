CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
LDFLAGS :=

SRC := src/server.c
BIN := tiny-redis

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN)

run: $(BIN)
	./$(BIN)

.PHONY: all clean run
