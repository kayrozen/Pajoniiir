CC = gcc
CFLAGS = -Wall -Wextra -std=c99 \
         -DREKORDBOX_PDB_STANDALONE_TEST \
         -I../../firmware/main-deck-jc1060/components/library/include

SRCS = test_pdb_playlists_jc1060.c \
       ../../firmware/main-deck-jc1060/components/library/rekordbox_pdb.c \
       ../../firmware/main-deck-jc1060/components/library/library_playlists.c

TARGET = test_pdb_playlists_jc1060

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) test_pdb_playlists_jc1060.pdb
