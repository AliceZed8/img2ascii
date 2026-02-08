CC = gcc
CFLAGS = -O3
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale

SOURCES = main.c
TARGET = img2ascii


all:
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

