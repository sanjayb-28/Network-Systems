#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define BUFFER_SIZE 1024

// Structure to pass arguments to the thread
typedef struct {
    int client_sock;
    char *directory;
} thread_args_t;

// Function to handle client requests
void *handle_client(void *args) {
    thread_args_t *t_args = (thread_args_t *)args;
    int sock = t_args->client_sock;
    char *dir = t_args->directory;
    free(t_args); // Free the allocated memory for arguments

    char buffer[BUFFER_SIZE];
    char command[16];
    char filename[256];
    
    // Read the command
    ssize_t n = read(sock, buffer, BUFFER_SIZE - 1);
    if (n <= 0) {
        close(sock);
        return NULL;
    }
    buffer[n] = '\0';

    // Simple parsing (expecting "COMMAND filename" or just "COMMAND")
    // Note: This is a basic protocol. You might need a more robust one.
    // For this assignment, let's assume the client sends "PUT filename size" or "GET filename" or "LIST"
    
    char *token = strtok(buffer, " \n");
    if (token == NULL) {
        close(sock);
        return NULL;
    }
    strncpy(command, token, sizeof(command) - 1);

    if (strcmp(command, "GET") == 0) {
        token = strtok(NULL, " \n");
        if (token) {
            strncpy(filename, token, sizeof(filename) - 1);
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
            
            FILE *f = fopen(filepath, "rb");
            if (f) {
                // Send "OK" then file content
                write(sock, "OK\n", 3);
                
                // Get file size
                fseek(f, 0, SEEK_END);
                long filesize = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                // Send file size? Or just stream? 
                // Let's send size first for robustness if client expects it, 
                // but if client just reads until close, that's fine too.
                // Let's stick to a simple protocol: OK\n<content>
                
                char file_buf[BUFFER_SIZE];
                size_t bytes_read;
                while ((bytes_read = fread(file_buf, 1, sizeof(file_buf), f)) > 0) {
                    write(sock, file_buf, bytes_read);
                }
                fclose(f);
            } else {
                write(sock, "NF\n", 3); // Not Found
            }
        }
    } else if (strcmp(command, "PUT") == 0) {
        token = strtok(NULL, " \n");
        if (token) {
            strncpy(filename, token, sizeof(filename) - 1);
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
            
            // Expecting size next? Or just read until close?
            // Usually PUT sends size. Let's assume the rest of the buffer (if any) is data
            // and we keep reading.
            // But wait, if we use a single connection for multiple requests, we need framing.
            // The assignment implies simple one-off or keep-alive. 
            // "Your DFS servers must handle multiple connections and service multiple DFCs concurrently"
            
            // Let's look at how we want to implement the client. 
            // Client: connect, send "PUT filename", send data, close.
            // Server: read "PUT filename", open file, write data until EOF.
            
            // But wait, the first read might contain part of the file data if it was sent quickly.
            // The strtok corrupted the buffer. We need to be careful.
            
            // Better protocol:
            // Client sends: "PUT filename\n"
            // Server sends: "READY\n"
            // Client sends: data...
            // Server closes or sends "OK\n"
            
            // Let's try to implement a slightly more robust handshake.
            write(sock, "READY\n", 6);
            
            FILE *f = fopen(filepath, "wb");
            if (f) {
                char file_buf[BUFFER_SIZE];
                ssize_t bytes_read;
                while ((bytes_read = read(sock, file_buf, sizeof(file_buf))) > 0) {
                    fwrite(file_buf, 1, bytes_read, f);
                }
                fclose(f);
            }
        }
    } else if (strcmp(command, "LIST") == 0) {
        DIR *d;
        struct dirent *dir_entry;
        d = opendir(dir);
        if (d) {
            write(sock, "OK\n", 3);
            while ((dir_entry = readdir(d)) != NULL) {
                if (dir_entry->d_type == DT_REG) { // Only regular files
                    write(sock, dir_entry->d_name, strlen(dir_entry->d_name));
                    write(sock, "\n", 1);
                }
            }
            closedir(d);
        } else {
            write(sock, "ERR\n", 4);
        }
    }

    close(sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <directory> <port>\n", argv[0]);
        exit(1);
    }

    char *directory = argv[1];
    int port = atoi(argv[2]);

    // Create directory if it doesn't exist
    struct stat st = {0};
    if (stat(directory, &st) == -1) {
        mkdir(directory, 0700);
    }

    int sockfd, newsockfd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t clilen;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }
    
    // Allow reuse of address
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    }

    listen(sockfd, 5);
    clilen = sizeof(cli_addr);

    while (1) {
        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0) {
            perror("ERROR on accept");
            continue;
        }

        pthread_t thread_id;
        thread_args_t *args = malloc(sizeof(thread_args_t));
        args->client_sock = newsockfd;
        args->directory = directory;

        if (pthread_create(&thread_id, NULL, handle_client, (void *)args) < 0) {
            perror("ERROR on creating thread");
            free(args);
            close(newsockfd);
        } else {
            pthread_detach(thread_id);
        }
    }

    close(sockfd);
    return 0;
}
