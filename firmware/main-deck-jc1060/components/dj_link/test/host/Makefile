# Host tests for the dj_link peer table, virtual-CDJ session and dbserver
# client (no ESP-IDF needed). test_dj_link_db runs a mock peer over localhost TCP.
# test_dj_link_pdb cross-checks the export.pdb lookup against the library
# parser built standalone.
CC ?= gcc
DJLINK = ../../../djlink
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -O2 -I../../include -I$(DJLINK)/include
LDFLAGS = -lm

SRC = ../../dj_link_state.c ../../dj_link_session.c $(wildcard $(DJLINK)/src/*.c)
DB_SRC = ../../dj_link_db.c $(wildcard $(DJLINK)/src/*.c)
LIBRARY = ../../../library
PDB_SRC = ../../dj_link_pdb.c $(LIBRARY)/rekordbox_pdb.c

all: test_dj_link_state test_dj_link_db test_dj_link_pdb

test_dj_link_state: test_dj_link_state.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_dj_link_state.c $(SRC) $(LDFLAGS)

test_dj_link_db: test_dj_link_db.c $(DB_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ test_dj_link_db.c $(DB_SRC) $(LDFLAGS)

test_dj_link_pdb: test_dj_link_pdb.c $(PDB_SRC)
	$(CC) $(CFLAGS) -DREKORDBOX_PDB_STANDALONE_TEST -I$(LIBRARY)/include -o $@ test_dj_link_pdb.c $(PDB_SRC) $(LDFLAGS)

run: all
	./test_dj_link_state
	./test_dj_link_db
	./test_dj_link_pdb

clean:
	rm -f test_dj_link_state test_dj_link_db test_dj_link_pdb

.PHONY: all run clean
