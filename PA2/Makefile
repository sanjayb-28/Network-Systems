# Compiler and flags
CC = gcc
CFLAGS = -g -Wall

SERVER = server/udp_server
CLIENT = client/udp_client

SERVER_SRC = udp_server.c
CLIENT_SRC = udp_client.c

TEST_FILES = foo1 foo2 foo3

all: $(SERVER) $(CLIENT)

$(SERVER): $(SERVER_SRC) $(TEST_FILES)
	@mkdir -p server
	@echo "Copying test files to server/ directory..."
	@cp $(TEST_FILES) server/
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC)

$(CLIENT): $(CLIENT_SRC)
	@mkdir -p client
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC)

clean:
	rm -rf server client