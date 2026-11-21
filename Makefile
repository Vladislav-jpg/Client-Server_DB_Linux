CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -O2
LDFLAGS = -pthread

all: server client

server: server.c db.c protocol.c db.h protocol.h
	$(CC) $(CFLAGS) -o server server.c db.c protocol.c $(LDFLAGS)

client: client.c protocol.c db.h protocol.h
	$(CC) $(CFLAGS) -o client client.c protocol.c

clean:
	rm -f server client *.db *.idx *.tmp

.PHONY: all clean
