CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c99 \
           -DHOT_CUE_STORE_STANDALONE_TEST \
           -I../../firmware/main-deck-jc1060/components/hot_cue_store/include

SRCS    = test_hot_cue_store_jc1060.c \
          ../../firmware/main-deck-jc1060/components/hot_cue_store/hot_cue_store.c

TARGET  = test_hot_cue_store_jc1060

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) test_hot_cue_store_jc1060.exe
