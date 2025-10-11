# Compiler and flags
CC = gcc
CFLAGS = -g -Wall

# Target executables
SERVER = server/udp_server
CLIENT = client/udp_client

# Source files
SERVER_SRC = udp_server.c
CLIENT_SRC = udp_client.c

# Test files that need to be in the server's directory
TEST_FILES = foo1 foo2 foo3

# Default target builds both executables
all: $(SERVER) $(CLIENT)

# Rule to build the server
# This now depends on the test files and copies them over before compiling.
$(SERVER): $(SERVER_SRC) $(TEST_FILES)
	@mkdir -p server
	@echo "Copying test files to server/ directory..."
	@cp $(TEST_FILES) server/
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC)

# Rule to build the client
$(CLIENT): $(CLIENT_SRC)
	@mkdir -p client
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC)

# Rule to clean up all generated files
clean:
	rm -rf server client