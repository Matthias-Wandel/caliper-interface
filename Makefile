# A makefile for compiling decode_Caliper.c and immediately run it
# useful for iterative debugging
CC = gcc
CFLAGS = -Wall -Wextra 
TARGET = decode_caliper 
all:$(TARGET) run

$(TARGET): decode_caliper.c
	$(CC) $(CFLAGS) -o $(TARGET) decode_caliper.c -lpigpio
	./$(TARGET)

run:
	./$(TARGET)

