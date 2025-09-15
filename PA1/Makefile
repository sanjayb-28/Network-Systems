CC = gcc

CFLAGS = -Wall -Wextra -std=c99 -g -pthread

EXE = server

all: $(EXE)

$(EXE): server.c
	$(CC) $(CFLAGS) -o $(EXE) server.c

clean:
	rm -f $(EXE)