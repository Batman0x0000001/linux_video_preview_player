CC=gcc
CFLAGS=-Wall -Wextra -O2 -g
PKG_CFLAGS=$(shell pkg-config --cflags sdl2 libavformat libavcodec libavutil libswscale)
PKG_LIBS=$(shell pkg-config --libs sdl2 libavformat libavcodec libavutil libswscale)

TARGET=linux_video_preview_player

SRCS=main.c \
     demux.c \
     decoder.c \
     frame_queue.c \
     display.c \
     clock.c \
     control.c

OBJS=$(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c app.h
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean