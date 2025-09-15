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

void *handle_connection(void *socket_ptr);
const char *get_content_type(const char *path);

// Main function: Sets up server and handles connections
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number.\n");
        return 1;
    }

    int server_fd;
    struct sockaddr_in server_addr;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Avoid "Address already in use" errors
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *client_socket_ptr = malloc(sizeof(int));

        if ((*client_socket_ptr = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len)) < 0) {
            perror("accept");
            free(client_socket_ptr);
            continue;
        }
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_connection, (void *)client_socket_ptr) != 0) {
            perror("pthread_create failed");
            close(*client_socket_ptr);
            free(client_socket_ptr);
        }
        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}

void *handle_connection(void *socket_ptr) {
    int client_socket = *(int*)socket_ptr;
    free(socket_ptr);

    // 10-second timeout
    struct timeval tv = {10, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    char buffer[BUFFER_SIZE];
    int keep_alive = 0;

    do {
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_received = read(client_socket, buffer, BUFFER_SIZE - 1);
        if (bytes_received <= 0) {
            break;
        }

        char method[16], uri[256], version[16];
        sscanf(buffer, "%s %s %s", method, uri, version);

        // HTTP/1.1 defaults to keep-alive
        keep_alive = (strcmp(version, "HTTP/1.1") == 0);
        if (strstr(buffer, "Connection: close")) {
            keep_alive = 0;
        }

        // Only handle GET requests
        if (strcmp(method, "GET") != 0) {
            char response[] = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
            write(client_socket, response, strlen(response));
            continue;
        }
        
        // Only support HTTP/1.0 and HTTP/1.1
        if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
            char response[] = "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 0\r\n\r\n";
            write(client_socket, response, strlen(response));
            continue;
        }

        // Default to index.html for root path
        char filepath[512];
        if (strcmp(uri, "/") == 0) {
            strcpy(uri, "/index.html");
        }
        sprintf(filepath, "www%s", uri);
        
        struct stat file_stat;
        if (stat(filepath, &file_stat) < 0) {
            // File not found
            char body[] = "<html><body><h1>404 Not Found</h1></body></html>";
            char header[256];
            sprintf(header, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", 
                    strlen(body));
            write(client_socket, header, strlen(header));
            write(client_socket, body, strlen(body));
            keep_alive = 0;
        } else {
            FILE *file = fopen(filepath, "rb");
            if (file == NULL) {
                // Permission denied or other error
                char body[] = "<html><body><h1>403 Forbidden</h1></body></html>";
                char header[256];
                sprintf(header, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", 
                        strlen(body));
                write(client_socket, header, strlen(header));
                write(client_socket, body, strlen(body));
                keep_alive = 0;
            } else {
                // Found the file, send it
                long file_size = file_stat.st_size;
                const char *content_type = get_content_type(filepath);

                char header[512];
                sprintf(header, "HTTP/1.1 200 OK\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %ld\r\n"
                               "Connection: %s\r\n\r\n",
                               content_type, file_size, 
                               keep_alive ? "keep-alive" : "close");
                
                write(client_socket, header, strlen(header));

                char file_buffer[BUFFER_SIZE];
                size_t bytes_read;
                while ((bytes_read = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
                    write(client_socket, file_buffer, bytes_read);
                }
                fclose(file);
            }
        }
    } while (keep_alive);

    close(client_socket);
    return NULL;
}

// Get MIME type based on file extension
const char *get_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) 
        return "application/octet-stream";

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