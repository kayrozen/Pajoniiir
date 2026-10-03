# Host tests for the dj_link peer table, virtual-CDJ session and dbserver
# client (no ESP-IDF needed). test_dj_link_db runs a mock peer over localhost TCP.
# test_dj_link_pdb cross-checks the export.pdb lookup against the library
# parser built standalone. test_dj_link_anlz (v300) writes ANLZ files from
# dbserver analysis blobs and reads them back with the library's ANLZ parser.
# test_dj_link_master (v305) covers the tempo master negotiation.
CC ?= gcc
DJLINK = ../../../djlink
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -O2 -I../../include -I$(DJLINK)/include
LDFLAGS = -lm

SRC = ../../dj_link_state.c ../../dj_link_session.c $(wildcard $(DJLINK)/src/*.c)
DB_SRC = ../../dj_link_db.c ../../dj_link_anlz.c $(wildcard $(DJLINK)/src/*.c)
LIBRARY = ../../../library
PDB_SRC = ../../dj_link_pdb.c $(LIBRARY)/rekordbox_pdb.c
ANLZ_SRC = ../../dj_link_anlz.c $(LIBRARY)/rekordbox_anlz.c

all: test_dj_link_state test_dj_link_master test_dj_link_db test_dj_link_pdb test_dj_link_anlz

test_dj_link_state: test_dj_link_state.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_dj_link_state.c $(SRC) $(LDFLAGS)

test_dj_link_master: test_dj_link_master.c ../../dj_link_master.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_dj_link_master.c ../../dj_link_master.c $(SRC) $(LDFLAGS)

test_dj_link_db: test_dj_link_db.c $(DB_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ test_dj_link_db.c $(DB_SRC) $(LDFLAGS)

test_dj_link_pdb: test_dj_link_pdb.c $(PDB_SRC)
	$(CC) $(CFLAGS) -DREKORDBOX_PDB_STANDALONE_TEST -I$(LIBRARY)/include -o $@ test_dj_link_pdb.c $(PDB_SRC) $(LDFLAGS)

test_dj_link_anlz: test_dj_link_anlz.c $(ANLZ_SRC)
	$(CC) $(CFLAGS) -DANLZ_STANDALONE_TEST -I$(LIBRARY)/include -o $@ test_dj_link_anlz.c $(ANLZ_SRC) $(LDFLAGS)

run: all
	./test_dj_link_state
	./test_dj_link_master
	./test_dj_link_db
	./test_dj_link_pdb
	./test_dj_link_anlz

clean:
	rm -f test_dj_link_state test_dj_link_master test_dj_link_db test_dj_link_pdb test_dj_link_anlz

.PHONY: all run clean
