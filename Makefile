CC = gcc
CFLAGS = -Wall -Wextra
LDLIBS = -lcontract

ctsup: src/main.c
	$(CC) $(CFLAGS) -o $@ src/main.c $(LDLIBS)

.PHONY: check
check:
	./tools/check ./src/*.c ./tools/* *.md Makefile

.PHONY: clean
clean:
	rm -f ctsup

