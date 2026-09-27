# Host tests for the esp-djlink codec (no ESP-IDF needed).
CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c99 -O2 -I../../include
LDFLAGS = -lm

SRC = $(wildcard ../../src/*.c)

djlink_test test2: djlink_test.c djlink_test2.c $(SRC)
	$(CC) $(CFLAGS) -o djlink_test djlink_test.c $(SRC) $(LDFLAGS)
	$(CC) $(CFLAGS) -o test2 djlink_test2.c $(SRC) $(LDFLAGS)

# NFS client against a mock player on 127.0.0.1 (POSIX sockets + pthread).
nfs_test: djlink_nfs_test.c $(SRC)
	$(CC) $(CFLAGS) -pthread -o nfs_test djlink_nfs_test.c $(SRC) $(LDFLAGS)

run: djlink_test test2 nfs_test
	./djlink_test && ./test2 && ./nfs_test

clean:
	rm -f djlink_test test2 nfs_test

.PHONY: run clean
