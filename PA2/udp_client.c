#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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

// Reliably RECEIVE a file ('get') using Go-Back-N
void handle_get_client(int sockfd, const char* filename, struct sockaddr_in server_addr) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("fopen");
        return;
    }
    printf("Receiving file '%s' with Go-Back-N...\n", filename);

    packet data_pkt;
    packet ack_pkt;
    ack_pkt.type = ACK;

    int expected_seq_num = 0;
    socklen_t slen = sizeof(server_addr);
    
    while(1) {
        recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0, (struct sockaddr *)&server_addr, &slen);
        
        // GBN Rule: Only accept packets that are in order.
        if (data_pkt.type == DATA && data_pkt.sequence_number == expected_seq_num) {
            if (data_pkt.payload_len == 0) {
                printf("\nTransfer complete.\n");
                ack_pkt.sequence_number = expected_seq_num;
                sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, slen);
                break; // EOF packet marks the end
            }
            fwrite(data_pkt.payload, 1, data_pkt.payload_len, file);
            printf(".");
            fflush(stdout);
            
            ack_pkt.sequence_number = expected_seq_num;
            sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, slen);
            expected_seq_num++;
        } else if (data_pkt.type == ERROR) {
            printf("\nError from server: %s\n", data_pkt.payload);
            break;
        } else {
            // Discard out-of-order packet, but re-send the ACK for the last good packet
            printf("\nDiscarding packet #%d, expecting #%d. Resending ACK #%d.\n", data_pkt.sequence_number, expected_seq_num, expected_seq_num - 1);
            ack_pkt.sequence_number = expected_seq_num - 1;
            sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, slen);
        }
    }
    fclose(file);
    if (data_pkt.type == ERROR) remove(filename); // Clean up partial file on error
}

// Reliably SEND a file ('put') using Go-Back-N
void handle_put_client(int sockfd, const char* filename, struct sockaddr_in server_addr) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("fopen");
        return;
    }

    // First, wait for the server to ACK our 'put' command
    packet ack_pkt;
    socklen_t slen = sizeof(server_addr);
    recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, &slen);
    if(ack_pkt.type != ACK || ack_pkt.sequence_number != -1) {
        printf("Server did not acknowledge PUT request. Aborting.\n");
        fclose(file);
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
        while (next_seq_num < base + WINDOW_SIZE && !file_done) {
            int win_idx = next_seq_num % WINDOW_SIZE;
            size_t bytes_read = fread(window[win_idx].payload, 1, PAYLOAD_SIZE, file);

            window[win_idx].type = DATA;
            window[win_idx].sequence_number = next_seq_num;
            window[win_idx].payload_len = bytes_read;
            
            if (bytes_read < PAYLOAD_SIZE) {
                file_done = 1;
            }
            sendto(sockfd, &window[win_idx], sizeof(packet), 0, (struct sockaddr *)&server_addr, slen);
            next_seq_num++;
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_MS * 1000;

        int ready_sockets = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        
        if (ready_sockets == 0) { // Timeout
            printf("Timeout on base %d, resending window.\n", base);
            for (int i = base; i < next_seq_num; i++) {
                sendto(sockfd, &window[i % WINDOW_SIZE], sizeof(packet), 0, (struct sockaddr *)&server_addr, slen);
            }
        } else { // ACK received
            recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, NULL, NULL);
            if (ack_pkt.type == ACK && ack_pkt.sequence_number >= base - 1) {
                base = ack_pkt.sequence_number + 1;
            }
        }
    }
    
    // Reliably send the final EOF packet
    packet eof_pkt;
    eof_pkt.type = DATA;
    eof_pkt.sequence_number = next_seq_num;
    eof_pkt.payload_len = 0;
    while(1) {
        sendto(sockfd, &eof_pkt, sizeof(eof_pkt), 0, (struct sockaddr *)&server_addr, slen);
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_MS * 1000;
        int ready_sockets = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready_sockets > 0) {
            packet final_ack;
            recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, NULL, NULL);
            if (final_ack.type == ACK && final_ack.sequence_number == next_seq_num) {
                break;
            }
        } else {
             printf("Timeout on EOF packet, resending.\n");
        }
    }

    fclose(file);
    printf("File transfer complete for '%s'.\n", filename);
}


int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(1);
    }
    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    int sockfd;
    struct sockaddr_in server_addr;
    char line_buffer[256];
    packet pkt, ack_pkt;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) die("socket() failed");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_aton(server_ip, &server_addr.sin_addr) == 0) die("inet_aton() failed");

    socklen_t slen = sizeof(server_addr);

    while(1) {
        printf("> ");
        fgets(line_buffer, sizeof(line_buffer), stdin);
        line_buffer[strcspn(line_buffer, "\n")] = 0;

        char *command = strtok(line_buffer, " ");
        char *filename = strtok(NULL, " ");
        if (!command) continue;

        memset(&pkt, 0, sizeof(pkt));

        if (strcmp(command, "ls") == 0) {
            pkt.type = LS;
            sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&server_addr, slen);
            recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, &slen);
            printf("Server response:\n%s\n", ack_pkt.payload);
        } else if (strcmp(command, "delete") == 0) {
            if (!filename) { printf("Usage: delete [file_name]\n"); continue; }
            pkt.type = DELETE;
            strncpy(pkt.payload, filename, PAYLOAD_SIZE - 1);
            pkt.payload_len = strlen(pkt.payload);
            sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&server_addr, slen);
            recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, &slen);
            printf("Server response:\n%s\n", ack_pkt.payload);
        } else if (strcmp(command, "get") == 0) {
            if (!filename) { printf("Usage: get [file_name]\n"); continue; }
            pkt.type = GET;
            strncpy(pkt.payload, filename, PAYLOAD_SIZE - 1);
            pkt.payload_len = strlen(pkt.payload);
            sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&server_addr, slen);
            handle_get_client(sockfd, filename, server_addr);
        } else if (strcmp(command, "put") == 0) {
            if (!filename) { printf("Usage: put [file_name]\n"); continue; }
            pkt.type = PUT;
            strncpy(pkt.payload, filename, PAYLOAD_SIZE - 1);
            pkt.payload_len = strlen(pkt.payload);
            sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&server_addr, slen);
            handle_put_client(sockfd, filename, server_addr);
        } else if (strcmp(command, "exit") == 0) {
            pkt.type = EXIT;
            sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&server_addr, slen);
            // Wait for the server's final ACK before we actually exit
            recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&server_addr, &slen);
            printf("Server acknowledged exit. Shutting down.\n");
            break; // Now we can break the loop
        } else {
            printf("Unknown command: %s\n", command);
        }
    }

    close(sockfd);
    return 0;
}