# Makefile for c-gui-app

CC = gcc
CFLAGS = -I../include -Wall -g
LDFLAGS = `pkg-config --cflags --libs gtk+-3.0`

SRC = ../src/main.c ../src/app.c ../src/gui.c
OBJ = $(SRC:.c=.o)
TARGET = c-gui-app

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean