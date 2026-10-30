CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS  = -lm
PORT    ?= 8080

all: calc

calc: src/server.c src/calc.c src/calc.h
	$(CC) $(CFLAGS) -o $@ src/server.c src/calc.c $(LDLIBS)

tests/test_calc: tests/test_calc.c src/calc.c src/calc.h
	$(CC) $(CFLAGS) -o $@ tests/test_calc.c src/calc.c $(LDLIBS)

test: tests/test_calc
	./tests/test_calc

run: calc
	./calc -p $(PORT)

clean:
	rm -f calc tests/test_calc

.PHONY: all test run clean
