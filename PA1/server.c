#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/time.h>

#define BUFFER_SIZE 8192

// Function Prototypes
void *handle_client(void *client_socket_ptr);
const char *get_mime_type(const char *file_path);

// Main function: sets up the socket and enters the main accept loop.
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0) {
        fprintf(stderr, "ERROR: Invalid port number.\n");
        return 1;
    }

    int server_sock;
    struct sockaddr_in server_addr;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("ERROR: Could not create socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR: Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 10) < 0) {
        perror("ERROR: Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d\n", port);

    while (1) {
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, NULL, NULL);

        if (*client_sock < 0) {
            perror("WARN: Accept failed");
            free(client_sock);
            continue;
        }
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client_sock) != 0) {
            perror("WARN: Could not create thread");
            close(*client_sock);
            free(client_sock);
        }
        pthread_detach(thread_id);
    }

    close(server_sock);
    return 0;
}

// Thread function: handles all communication with a single client.
void *handle_client(void *client_socket_ptr) {
    int client_sock = *(int*)client_socket_ptr;
    free(client_socket_ptr);

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[BUFFER_SIZE];
    
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        
        ssize_t bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
        if (bytes_read <= 0) {
            break;
        }

        char method[16], uri[256], version[16];
        sscanf(buffer, "%s %s %s", method, uri, version);

        int keep_alive;
        if (strcmp(version, "HTTP/1.1") == 0) {
            keep_alive = 1;
        } else {
            keep_alive = 0;
        }
        if (strstr(buffer, "Connection: close")) {
            keep_alive = 0;
        }

        int is_valid_request = 1;
        if (strcmp(method, "GET") != 0) {
            char response[] = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
            write(client_sock, response, strlen(response));
            is_valid_request = 0;
        } else if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
            char response[] = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
            write(client_sock, response, strlen(response));
            is_valid_request = 0;
        }

        if (is_valid_request) {
            char filepath[512];
            if (strcmp(uri, "/") == 0) {
                strcpy(filepath, "www/index.html");
            } else {
                sprintf(filepath, "www%s", uri);
            }
            
            FILE *file = fopen(filepath, "rb");
            if (file == NULL) {
                char response[] = "HTTP/1.1 404 Not Found\r\n"
                                  "Content-Type: text/html\r\n"
                                  "\r\n"
                                  "<h1>404 Not Found</h1>";
                write(client_sock, response, strlen(response));
                keep_alive = 0;
            } else {
                struct stat file_stat;
                stat(filepath, &file_stat);
                long file_size = file_stat.st_size;

                const char *mime_type = get_mime_type(filepath);

                char header[512];
                sprintf(header, "HTTP/1.1 200 OK\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %ld\r\n"
                                "Connection: %s\r\n\r\n",
                                mime_type, file_size, 
                                keep_alive ? "keep-alive" : "close");
                
                write(client_sock, header, strlen(header));

                char file_buffer[BUFFER_SIZE];
                size_t chunk_size;
                while ((chunk_size = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
                    write(client_sock, file_buffer, chunk_size);
                }
                fclose(file);
            }
        }

        if (!keep_alive) {
            break;
        }
    }

    close(client_sock);
    return NULL;
}

// Helper function: returns the MIME type for a given file path.
const char *get_mime_type(const char *file_path) {
    const char *dot = strrchr(file_path, '.');
    if (!dot) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".txt") == 0) return "text/plain";
    
    return "application/octet-stream";
}