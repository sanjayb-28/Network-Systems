#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>

// --- GBN Protocol Definition ---
#define PAYLOAD_SIZE 1024
#define WINDOW_SIZE 5
#define TIMEOUT_MS 500

typedef enum {
    LS, DELETE, GET, PUT, EXIT,
    DATA, ACK, ERROR
} packet_type;

typedef struct {
    packet_type type;
    int sequence_number;
    size_t payload_len;
    char payload[PAYLOAD_SIZE];
} packet;
// --- End of GBN Protocol Definition ---

void die(char *s) {
    perror(s);
    exit(1);
}

// Handler for the 'ls' command
void handle_ls(int sockfd, struct sockaddr_in client_addr, socklen_t slen) {
    DIR *d;
    struct dirent *dir;
    char file_list[PAYLOAD_SIZE] = {0};
    packet res_pkt;

    d = opendir(".");
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
                if (strlen(file_list) + strlen(dir->d_name) + 2 < PAYLOAD_SIZE) {
                    strcat(file_list, dir->d_name);
                    strcat(file_list, "\n");
                }
            }
        }
        closedir(d);
    }

    res_pkt.type = ACK;
    strncpy(res_pkt.payload, file_list, PAYLOAD_SIZE - 1);
    res_pkt.payload_len = strlen(res_pkt.payload);
    sendto(sockfd, &res_pkt, sizeof(res_pkt), 0, (struct sockaddr *)&client_addr, slen);
}

// Handler for the 'delete' command
void handle_delete(int sockfd, const char* filename, struct sockaddr_in client_addr, socklen_t slen) {
    packet res_pkt;
    if (remove(filename) == 0) {
        printf("Deleted file: %s\n", filename);
        res_pkt.type = ACK;
        strncpy(res_pkt.payload, "File deleted successfully.", PAYLOAD_SIZE);
    } else {
        perror("remove");
        res_pkt.type = ERROR;
        strncpy(res_pkt.payload, "Error: Failed to delete file.", PAYLOAD_SIZE);
    }
    res_pkt.payload_len = strlen(res_pkt.payload);
    sendto(sockfd, &res_pkt, sizeof(res_pkt), 0, (struct sockaddr *)&client_addr, slen);
}


// Reliably SEND a file ('get') using Go-Back-N
void handle_get(int sockfd, const char* filename, struct sockaddr_in client_addr, socklen_t slen) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        packet err_pkt;
        err_pkt.type = ERROR;
        strncpy(err_pkt.payload, "File not found.", PAYLOAD_SIZE);
        err_pkt.payload_len = strlen(err_pkt.payload);
        sendto(sockfd, &err_pkt, sizeof(err_pkt), 0, (struct sockaddr *)&client_addr, slen);
        return;
    }

    printf("Sending file '%s' with Go-Back-N...\n", filename);

    packet window[WINDOW_SIZE];
    int base = 0;
    int next_seq_num = 0;
    int file_done = 0;

    fd_set readfds;
    struct timeval timeout;

    while (!file_done || base < next_seq_num) {
        // Phase 1: Send packets until the window is full
        while (next_seq_num < base + WINDOW_SIZE && !file_done) {
            int win_idx = next_seq_num % WINDOW_SIZE;
            size_t bytes_read = fread(window[win_idx].payload, 1, PAYLOAD_SIZE, file);

            window[win_idx].type = DATA;
            window[win_idx].sequence_number = next_seq_num;
            window[win_idx].payload_len = bytes_read;
            
            if (bytes_read < PAYLOAD_SIZE) {
                file_done = 1; // This was the last chunk of the file
            }
            sendto(sockfd, &window[win_idx], sizeof(packet), 0, (struct sockaddr *)&client_addr, slen);
            next_seq_num++;
        }

        // Phase 2: Wait for an ACK or timeout
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_MS * 1000;

        int ready_sockets = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        
        if (ready_sockets == 0) { // Timeout
            printf("Timeout on base %d, resending window.\n", base);
            for (int i = base; i < next_seq_num; i++) {
                sendto(sockfd, &window[i % WINDOW_SIZE], sizeof(packet), 0, (struct sockaddr *)&client_addr, slen);
            }
        } else { // ACK received
            packet ack_pkt;
            recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, NULL, NULL);
            if (ack_pkt.type == ACK && ack_pkt.sequence_number >= base - 1) {
                // Move window base forward
                base = ack_pkt.sequence_number + 1;
            }
        }
    }
    
    // Reliably send the final EOF packet (0-length payload)
    packet eof_pkt;
    eof_pkt.type = DATA;
    eof_pkt.sequence_number = next_seq_num;
    eof_pkt.payload_len = 0;
    
    while(1) {
        sendto(sockfd, &eof_pkt, sizeof(eof_pkt), 0, (struct sockaddr *)&client_addr, slen);
        
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_MS * 1000;
        
        int ready_sockets = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready_sockets > 0) {
            packet final_ack;
            recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, NULL, NULL);
            if (final_ack.type == ACK && final_ack.sequence_number == next_seq_num) {
                break; // Final ACK received, we are done
            }
        } else {
            printf("Timeout on EOF packet, resending.\n");
        }
    }

    fclose(file);
    printf("File transfer complete for '%s'.\n", filename);
}


// Reliably RECEIVE a file ('put') using Go-Back-N
void handle_put(int sockfd, const char* filename, struct sockaddr_in client_addr, socklen_t slen) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("fopen");
        // Still need to ACK the put request to unblock the client, even on error
        packet err_ack;
        err_ack.type = ACK;
        err_ack.sequence_number = -1;
        sendto(sockfd, &err_ack, sizeof(err_ack), 0, (struct sockaddr*)&client_addr, slen);
        return;
    }
    printf("Receiving file '%s' with Go-Back-N...\n", filename);
    
    // Send ACK for the initial PUT request to start the transfer
    packet ack_pkt;
    ack_pkt.type = ACK;
    ack_pkt.sequence_number = -1; // Special ACK for the command, not data
    sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&client_addr, slen);
    
    packet data_pkt;
    int expected_seq_num = 0;
    while(1) {
        recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0, (struct sockaddr *)&client_addr, &slen);
        
        if (data_pkt.type == DATA && data_pkt.sequence_number == expected_seq_num) {
            // Correct packet received
            if (data_pkt.payload_len == 0) {
                 ack_pkt.sequence_number = expected_seq_num; // ACK the EOF packet
                 sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&client_addr, slen);
                 break; // Transfer is complete
            }
            fwrite(data_pkt.payload, 1, data_pkt.payload_len, file);
            ack_pkt.sequence_number = expected_seq_num;
            sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&client_addr, slen);
            expected_seq_num++;
        } else {
            // Wrong packet received, discard it and re-send the ACK for the last good packet
            printf("Discarding packet #%d, expecting #%d. Resending ACK #%d.\n", data_pkt.sequence_number, expected_seq_num, expected_seq_num - 1);
            ack_pkt.sequence_number = expected_seq_num - 1;
            sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&client_addr, slen);
        }
    }
    fclose(file);
    printf("File reception complete for '%s'.\n", filename);
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t slen = sizeof(client_addr);
    packet recv_pkt;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) die("socket() failed");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) die("bind() failed");

    printf("Server listening on port %d...\n", port);

    while (1) {
        recvfrom(sockfd, &recv_pkt, sizeof(recv_pkt), 0, (struct sockaddr *)&client_addr, &slen);
        printf("Received command '%d' from %s:%d\n", recv_pkt.type, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        switch (recv_pkt.type) {
            case LS:
                handle_ls(sockfd, client_addr, slen);
                break;
            case DELETE:
                handle_delete(sockfd, recv_pkt.payload, client_addr, slen);
                break;
            case GET:
                handle_get(sockfd, recv_pkt.payload, client_addr, slen);
                break;
            case PUT:
                handle_put(sockfd, recv_pkt.payload, client_addr, slen);
                break;
            case EXIT:
                printf("Exit command received. Shutting down.\n");
                packet goodbye_pkt;
                goodbye_pkt.type = ACK;
                strncpy(goodbye_pkt.payload, "Goodbye!", PAYLOAD_SIZE);
                goodbye_pkt.payload_len = strlen(goodbye_pkt.payload);
                sendto(sockfd, &goodbye_pkt, sizeof(goodbye_pkt), 0, (struct sockaddr *)&client_addr, slen);
                close(sockfd);
                exit(0);
            default:
                printf("Unknown command received.\n");
                break;
        }
    }
    return 0;
}