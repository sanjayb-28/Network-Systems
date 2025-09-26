/*
 * =====================================================================================
 *
 * Filename:  server.c
 *
 * Description:  A simple multi-threaded HTTP web server in C.
 *
 * =====================================================================================
 */

// These are preprocessor directives that include standard C library headers.
// Each header provides functions and type definitions for a specific purpose.

#include <stdio.h>      // Provides standard input/output functions like printf(), fprintf(), fopen(), fread().
#include <stdlib.h>     // Provides functions for memory allocation (malloc(), free()), string conversion (atoi()), and process control (exit()).
#include <string.h>     // Provides string manipulation functions like strcpy(), sprintf(), strcmp(), strstr().
#include <unistd.h>     // Provides access to the POSIX operating system API, including read(), write(), and close().
#include <sys/socket.h> // The main header for socket programming. It provides the core functions: socket(), bind(), listen(), accept(), setsockopt().
#include <netinet/in.h> // Provides internet address family structures like 'struct sockaddr_in' and functions like htons().
#include <sys/stat.h>   // Provides the stat() function, which we use to get file information like its size.
#include <pthread.h>    // The POSIX threads library header, which provides functions for creating and managing threads (pthread_create(), pthread_detach()).
#include <sys/time.h>   // Provides time-related structures like 'struct timeval', used here for setting socket timeouts.


// A preprocessor macro to define a constant for our buffer size.
// Using a macro makes the code cleaner and easier to modify.
#define BUFFER_SIZE 8192

// Function Prototypes: These declare our functions before they are defined.
// This allows main() to call handle_client() even though its full definition appears later.
void *handle_client(void *client_socket_ptr);
const char *get_mime_type(const char *file_path);


// The main function is the entry point of our program.
int main(int argc, char *argv[]) {
    // --- 1. Argument Parsing ---
    // argc is the argument count, and argv is an array of argument strings.
    // We expect one argument: the port number. argv[0] is the program name.
    if (argc != 2) {
        // If the wrong number of arguments is provided, print a usage message to standard error.
        fprintf(stderr, "USAGE: %s <port>\n", argv[0]);
        return 1; // Exit with a non-zero status code to indicate an error.
    }

    // Convert the port number argument from a string to an integer.
    int port = atoi(argv[1]);
    // Validate that the port number is positive.
    if (port <= 0) {
        fprintf(stderr, "ERROR: Invalid port number.\n");
        return 1;
    }

    // --- 2. Socket Creation and Setup ---
    int server_sock; // This integer will be our listening socket's file descriptor.
    struct sockaddr_in server_addr; // A struct to hold our server's address information (IP, port, etc.).

    // Create a new socket.
    // AF_INET: Specifies the address family as IPv4.
    // SOCK_STREAM: Specifies the socket type as TCP (a reliable, connection-oriented stream).
    // 0: Specifies the protocol, letting the OS choose the default for SOCK_STREAM, which is TCP.
    // It returns a file descriptor (an integer handle) for the new socket.
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("ERROR: Could not create socket"); // perror prints our message plus the system error message.
        exit(EXIT_FAILURE); // Exit the program immediately.
    }

    // Set a socket option to allow the address to be reused immediately after the server is stopped.
    // This is very helpful during development to avoid "Address already in use" errors.
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // --- 3. Binding the Socket to an Address and Port ---
    // Zero out the server address struct to start clean.
    // Fills a block of memory with a specific byte value.
    memset(&server_addr, 0, sizeof(server_addr));
    // Set the address family to IPv4.
    server_addr.sin_family = AF_INET;
    // Set the IP address. INADDR_ANY means the server will accept connections on any available network interface.
    server_addr.sin_addr.s_addr = INADDR_ANY;
    // Set the port number. htons() ("Host to Network Short") converts the port number to network byte order.
    // This is crucial for interoperability between different computer architectures.
    server_addr.sin_port = htons(port);

    // Bind the socket file descriptor to the address and port we just configured.
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR: Bind failed");
        exit(EXIT_FAILURE);
    }

    // --- 4. Listening for Connections ---
    // Put the server socket into a passive listening state, ready to accept incoming client connections.
    // The '10' is the backlog, the maximum number of pending connections the kernel should queue.
    if (listen(server_sock, 10) < 0) {
        perror("ERROR: Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d\n", port);

    // --- 5. The Main Accept Loop ---
    // This infinite loop continuously waits for and accepts new client connections.
    while (1) {
        // We need to pass the client socket descriptor to a new thread. To do this safely,
        // we allocate memory on the heap for it.
        int *client_sock = malloc(sizeof(int));

        // The accept() call is a BLOCKING call. The program will pause here until a client connects.
        // When a connection is made, accept() creates a NEW socket for that specific client
        // and returns its file descriptor.
        *client_sock = accept(server_sock, NULL, NULL);

        if (*client_sock < 0) {
            perror("WARN: Accept failed");
            free(client_sock); // Free the memory we allocated if accept fails.
            continue; // Go to the next iteration of the loop.
        }
        
        // Create a new thread to handle the client's request concurrently.
        pthread_t thread_id;
        // pthread_create() starts a new thread that will execute the 'handle_client' function.
        // We pass it a pointer to the newly created client socket.
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client_sock) != 0) {
            perror("WARN: Could not create thread");
            close(*client_sock); // If thread creation fails, close the socket...
            free(client_sock);   // ...and free the memory.
        }
        
        // Detach the thread. This tells the OS to automatically clean up the thread's resources
        // when it finishes, so we don't have to manually call pthread_join().
        pthread_detach(thread_id);
    }

    // This code is technically unreachable because of the infinite loop, but it's good practice.
    close(server_sock);
    return 0;
}


// This is the function that each new thread will execute.
// It handles all communication with a single client.
void *handle_client(void *client_socket_ptr) {
    // --- 1. Thread Setup ---
    // Get the client socket descriptor from the argument pointer.
    int client_sock = *(int*)client_socket_ptr;
    // We now have our own copy of the socket descriptor, so we can free the heap memory
    // that was allocated in the main loop. This prevents memory leaks.
    free(client_socket_ptr);

    // --- 2. Set Timeout for Persistent Connections ---
    // Create a timeval struct to define a 10-second timeout.
    struct timeval timeout;
    timeout.tv_sec = 10;  // 10 seconds
    timeout.tv_usec = 0;   // 0 microseconds
    // Set a receive timeout on the client socket. If a read() call blocks for more than 10 seconds,
    // it will fail, allowing us to close the idle connection.
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // A buffer to store the client's HTTP request.
    char buffer[BUFFER_SIZE];
    
    // --- 3. The Persistent Connection Loop ---
    // This loop allows a single client to make multiple requests over the same connection (HTTP Keep-Alive).
    while (1) {
        // Clear the buffer before reading a new request.
        memset(buffer, 0, BUFFER_SIZE);
        
        // Read the client's request from the socket into the buffer.
        // This is a BLOCKING call (with a 10-second timeout).
        // It returns the number of bytes read.
        ssize_t bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
        
        // If read() returns 0 or a negative number, it means the client disconnected,
        // an error occurred, or our timeout was hit. In any case, we should close the connection.
        if (bytes_read <= 0) {
            break; // Exit the while loop.
        }

        // --- 4. Parse the HTTP Request ---
        // Declare character arrays to store the parsed components of the request line.
        char method[16], uri[256], version[16];
        // sscanf parses the request line (e.g., "GET /index.html HTTP/1.1") from the buffer.
        sscanf(buffer, "%s %s %s", method, uri, version);

        // --- 5. Handle Keep-Alive Logic ---
        int keep_alive;
        // HTTP/1.1 defaults to persistent connections.
        if (strcmp(version, "HTTP/1.1") == 0) {
            keep_alive = 1;
        } else {
            keep_alive = 0; // HTTP/1.0 and others default to closing the connection.
        }
        // However, if the client explicitly sends a "Connection: close" header, we must honor it.
        if (strstr(buffer, "Connection: close")) {
            keep_alive = 0;
        }

        // --- 6. Validate the Request and Handle Errors ---
        int is_valid_request = 1; // A flag to track if we should proceed to serve a file.
        // We only support the GET method.
        if (strcmp(method, "GET") != 0) {
            char response[] = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
            write(client_sock, response, strlen(response)); // Send a 405 response.
            is_valid_request = 0;
        // We only support HTTP/1.0 and HTTP/1.1.
        } else if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
            char response[] = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
            write(client_sock, response, strlen(response)); // Send a 505 response.
            is_valid_request = 0;
        }

        // --- 7. Serve the File (if the request was valid) ---
        if (is_valid_request) {
            char filepath[512];
            // If the URI is just "/", serve the default index.html page.
            if (strcmp(uri, "/") == 0) {
                strcpy(filepath, "www/index.html");
            } else {
                // Otherwise, prepend "www" to the URI to form the local file path.
                sprintf(filepath, "www%s", uri);
            }
            
            // Try to open the file for reading in binary mode ("rb").
            // Using "rb" is crucial to handle images and other non-text files correctly.
            FILE *file = fopen(filepath, "rb");
            if (file == NULL) {
                // If fopen returns NULL, the file was not found. Send a 404 response.
                char response[] = "HTTP/1.1 404 Not Found\r\n"
                                  "Content-Type: text/html\r\n"
                                  "\r\n"
                                  "<h1>404 Not Found</h1>";
                write(client_sock, response, strlen(response));
                keep_alive = 0; // It's common practice to close the connection on an error.
            } else {
                // If the file was opened successfully, prepare a 200 OK response.
                struct stat file_stat;
                stat(filepath, &file_stat); // Use stat() to get file metadata.
                long file_size = file_stat.st_size; // Get the exact size of the file in bytes.

                // Get the appropriate MIME type for the Content-Type header.
                const char *mime_type = get_mime_type(filepath);

                // Build the HTTP response headers in a character array.
                char header[512];
                sprintf(header, "HTTP/1.1 200 OK\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %ld\r\n" // The Content-Length is essential for the client.
                                "Connection: %s\r\n\r\n", // Tell the client if we're keeping the connection open.
                                mime_type, file_size, 
                                keep_alive ? "keep-alive" : "close");
                
                // Send the formatted headers to the client.
                write(client_sock, header, strlen(header));

                // Now, send the actual file content in chunks.
                char file_buffer[BUFFER_SIZE];
                size_t chunk_size;
                // Loop: read a chunk from the file, then write that chunk to the socket.
                while ((chunk_size = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
                    write(client_sock, file_buffer, chunk_size);
                }
                // Close the file when we're done.
                fclose(file);
            }
        }

        // After handling the request, check if we should close the connection.
        if (!keep_alive) {
            break; // Exit the while loop.
        }
    }

    // --- 8. Close the Connection ---
    // This code is reached when the while loop breaks.
    close(client_sock); // Close the client's socket and free up its resources.
    return NULL; // End the thread.
}

// A simple helper function to determine the MIME type of a file based on its extension.
const char *get_mime_type(const char *file_path) {
    // strrchr finds the last occurrence of a character ('.') in a string.
    const char *dot = strrchr(file_path, '.');
    // If there's no dot, we can't determine the type, so we use a generic default.
    if (!dot) return "application/octet-stream";

    // Compare the extension against a list of known types.
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".txt") == 0) return "text/plain";
    
    // If the extension is not in our list, use the generic binary stream type.
    return "application/octet-stream";
}




//CURL:

//success                                   curl http://localhost:8888/index.html
// verbose success                          curl -v http://localhost:8888/index.html
// 404                                      curl -v http://localhost:8888/random.html
// 405                                      curl -v -X POST http://localhost:8888/index.html
// connection close                         curl -v --http1.1 --header "Connection: close" http://localhost:8888/index.html
// images                                   curl http://localhost:8888/images/exam.gif -o downloaded_exam.gif



//Netcat:

//success                                   printf "GET /index.html HTTP/2.0\r\nHost: localhost\r\n\r\n" | nc localhost 8888
// 505                                      printf "GET /index.html HTTP/2.0\r\nHost: localhost\r\n\r\n" | nc localhost 8888
// persistent                               (echo -en "GET /index.html HTTP/1.1\r\nConnection: keep-alive\r\n\r\n"; sleep 2; echo -en "GET /index.html HTTP/1.1\r\nConnection: close\r\n\r\n") | nc localhost 8888

// run grading script:                      python3 grade.py --exe ./server --port 8888 --class-code 5273