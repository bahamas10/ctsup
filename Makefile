CC = gcc
CFLAGS = -Wall -Wextra
LDLIBS = -lcontract

ctsup: src/main.c
	$(CC) $(CFLAGS) -o $@ src/main.c $(LDLIBS)

.PHONY: check
check:
	./tools/check ./src/*.c ./tests/* ./tests/fixtures/*/* ./tools/* *.md \
	    Makefile

.PHONY: test
test: ctsup
	cd ./tests && ./runtests

.PHONY: clean
clean:
	rm -f ctsup
