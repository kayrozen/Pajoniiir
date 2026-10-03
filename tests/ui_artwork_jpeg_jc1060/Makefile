CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Werror -std=c99 \
         -I../../firmware/main-deck-jc1060/components/ui/include

SRCS = test_ui_artwork_jpeg.c \
       ../../firmware/main-deck-jc1060/components/ui/ui_artwork_jpeg.c

TARGET = test_ui_artwork_jpeg

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# Optional: make test JPG="cover.jpg other.jpg" (prints each verdict)
test: $(TARGET)
	./$(TARGET) $(JPG)

clean:
	rm -f $(TARGET)
