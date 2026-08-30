CC = gcc
CFLAGS = -Wall -Wextra
LDLIBS = -lcontract

ctsup: src/main.c
	$(CC) $(CFLAGS) -o $@ src/main.c $(LDLIBS)

.PHONY: clean
clean:
	rm -f ctsup

