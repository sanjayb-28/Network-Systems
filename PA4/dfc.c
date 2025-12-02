#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/md5.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <libgen.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

#define MAX_SERVERS 4
#define BUFFER_SIZE 1024

typedef struct {
    char name[16];
    char ip[32];
    int port;
} ServerConfig;

ServerConfig servers[MAX_SERVERS];
int num_servers = 0;

void read_config() {
    FILE *fp = fopen("dfc.conf", "r");
    if (!fp) {
        perror("Error opening dfc.conf");
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "server", 6) == 0) {
            char s_name[16], s_addr[32];
            sscanf(line, "server %s %s", s_name, s_addr);
            char *ip = strtok(s_addr, ":");
            char *port_str = strtok(NULL, ":");
            
            strcpy(servers[num_servers].name, s_name);
            strcpy(servers[num_servers].ip, ip);
            servers[num_servers].port = atoi(port_str);
            num_servers++;
        }
    }
    fclose(fp);
}

int get_hash_mod(const char *filename) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)filename, strlen(filename), digest);
    // Use the first byte? Or sum? Assignment says "hash the filename and then apply a modulus"
    // Usually standard is to treat hash as a number. 
    // Let's use the whole hash or a portion. 
    // Let's treat the first few bytes as an integer.
    unsigned int hash_val = 0;
    // Let's just sum them up or use the first 4 bytes.
    // A common way is to take the first 4 bytes as an integer.
    memcpy(&hash_val, digest, sizeof(unsigned int));
    return hash_val % 4;
}

// Table from assignment
// x    DFS1    DFS2    DFS3    DFS4
// 0    (1,2)   (2,3)   (3,4)   (4,1)
// 1    (4,1)   (1,2)   (2,3)   (3,4)
// 2    (3,4)   (4,1)   (1,2)   (2,3)
// 3    (2,3)   (3,4)   (4,1)   (1,2)

// Map: table[x][server_idx] -> pairs to store
// server_idx 0=DFS1, 1=DFS2, 2=DFS3, 3=DFS4
// Pairs: 1=(P1,P2), 2=(P2,P3), 3=(P3,P4), 4=(P4,P1)
// Wait, the table says:
// DFS1 stores (1,2) means P1 and P2.
// So let's map: table[x][server_idx] -> {chunk_a, chunk_b}
int distribution[4][4][2] = {
    {{1, 2}, {2, 3}, {3, 4}, {4, 1}}, // x=0
    {{4, 1}, {1, 2}, {2, 3}, {3, 4}}, // x=1
    {{3, 4}, {4, 1}, {1, 2}, {2, 3}}, // x=2
    {{2, 3}, {3, 4}, {4, 1}, {1, 2}}  // x=3
};

int connect_to_server(int server_idx) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(servers[server_idx].port);
    
    if(inet_pton(AF_INET, servers[server_idx].ip, &serv_addr.sin_addr)<=0) {
        close(sockfd);
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set myset;
            struct timeval tv;
            FD_ZERO(&myset);
            FD_SET(sockfd, &myset);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(sockfd + 1, NULL, &myset, NULL, &tv) > 0) {
                int so_error;
                socklen_t len = sizeof so_error;
                getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    // Connected
                    fcntl(sockfd, F_SETFL, flags); // Restore blocking
                    return sockfd;
                }
            }
        }
    } else {
        // Connected immediately (unlikely for TCP but possible)
        fcntl(sockfd, F_SETFL, flags);
        return sockfd;
    }

    close(sockfd);
    return -1;
}

void put_file(const char *filepath_in) {
    FILE *f = fopen(filepath_in, "rb");
    if (!f) {
        printf("%s put failed\n", filepath_in);
        return;
    }
    
    // Use basename for storage and hashing
    char *path_dup = strdup(filepath_in);
    char *filename = basename(path_dup);

    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = malloc(filesize);
    fread(buffer, 1, filesize, f);
    fclose(f);

    // Split into 4 chunks
    long chunk_size = filesize / 4;
    long last_chunk_size = filesize - (chunk_size * 3);
    
    char *chunks[4];
    long chunk_sizes[4];
    
    chunks[0] = buffer;
    chunk_sizes[0] = chunk_size;
    chunks[1] = buffer + chunk_size;
    chunk_sizes[1] = chunk_size;
    chunks[2] = buffer + 2 * chunk_size;
    chunk_sizes[2] = chunk_size;
    chunks[3] = buffer + 3 * chunk_size;
    chunk_sizes[3] = last_chunk_size;

    int x = get_hash_mod(filename);
    
    // We need to upload to all 4 servers if possible.
    // The assignment says: "If there are not enough servers to store the file reliably, your program should respond with <filename> put failed."
    // What is "reliably"?
    // "The stored file now has (limited) redundancy - one failed server will not affect the integrity of the file."
    // If we can store all pairs, we are good.
    // If one server is down, we miss 2 pairs.
    // Wait, if DFS1 is down (stores 1,2), we still have (4,1) on DFS2, (2,3) on DFS2... wait.
    // x=0:
    // DFS1: 1,2
    // DFS2: 2,3
    // DFS3: 3,4
    // DFS4: 4,1
    // If DFS1 down: we miss P1 copy 1, P2 copy 1.
    // But DFS2 has P2 copy 2. DFS4 has P1 copy 2.
    // So if 1 server is down, we still have all chunks.
    // So "reliably" probably means at least 3 servers are up?
    // Or maybe it just means "try to put to all, if fail too many, report error".
    // Let's try to put to all available servers.
    
    int success_count = 0;
    for (int i = 0; i < num_servers; i++) {
        int sock = connect_to_server(i);
        if (sock < 0) continue;

        // Determine which chunks to send
        int c1_idx = distribution[x][i][0] - 1; // 0-indexed
        int c2_idx = distribution[x][i][1] - 1;

        // Send Chunk 1
        // Protocol: PUT filename.partX
        // Wait, we should probably store them as filename.1, filename.2 etc on the server?
        // Or just store them with unique names?
        // The assignment doesn't specify server storage format, but "Each DFS server should have its own directory".
        // If we store "file.txt", we overwrite?
        // We should store chunks. "splits the file into 4 equal length chunks P1, P2, P3, P4... uploads the pairs"
        // So on DFS1 (x=0), we store P1 and P2.
        // Let's name them filename.1, filename.2
        
        // Helper to send one chunk
        char chunk_name[300];
        char resp[16];
        
        // Send Chunk 1
        snprintf(chunk_name, sizeof(chunk_name), ".%s.%d", filename, c1_idx + 1);
        dprintf(sock, "PUT %s\n", chunk_name);
        
        // Wait for READY
        int n = read(sock, resp, sizeof(resp));
        if (n > 0 && strncmp(resp, "READY", 5) == 0) {
            write(sock, chunks[c1_idx], chunk_sizes[c1_idx]);
        }
        close(sock);
        
        // Reconnect for Chunk 2 (Simple approach)
        sock = connect_to_server(i);
        if (sock < 0) continue;
        
        snprintf(chunk_name, sizeof(chunk_name), ".%s.%d", filename, c2_idx + 1);
        dprintf(sock, "PUT %s\n", chunk_name);
        
        n = read(sock, resp, sizeof(resp));
        if (n > 0 && strncmp(resp, "READY", 5) == 0) {
            write(sock, chunks[c2_idx], chunk_sizes[c2_idx]);
        }
        close(sock);
        
        success_count++;
    }
    
    free(buffer);
    
    if (success_count < 3) { 
        printf("%s put failed\n", filepath_in);
    } else {
        printf("%s put success\n", filepath_in);
    }
    free(path_dup);
}

void list_files() {
    // Map: filename -> [chunk1_present, chunk2_present, chunk3_present, chunk4_present]
    // But we need to handle many files.
    // Let's just get the list from all servers and aggregate.
    
    struct FileInfo {
        char name[256];
        int chunks[4]; // 0 or 1
        struct FileInfo *next;
    };
    struct FileInfo *head = NULL;

    for (int i = 0; i < num_servers; i++) {
        int sock = connect_to_server(i);
        if (sock < 0) continue;

        dprintf(sock, "LIST\n");
        
        FILE *sock_fp = fdopen(dup(sock), "r");
        char line[256];
        if (sock_fp) {
            if (fgets(line, sizeof(line), sock_fp)) {
                if (strncmp(line, "OK", 2) == 0) {
                    while (fgets(line, sizeof(line), sock_fp)) {
                        // Line is ".filename.chunkID"
                        char *p = line;
                        // Strip newline
                        line[strcspn(line, "\n")] = 0;
                        
                        if (p[0] == '.') {
                            p++; // skip dot
                            char *last_dot = strrchr(p, '.');
                            if (last_dot) {
                                *last_dot = '\0';
                                int chunk_id = atoi(last_dot + 1);
                                char *fname = p;
                                
                                // Add to list
                                struct FileInfo *curr = head;
                                int found = 0;
                                while (curr) {
                                    if (strcmp(curr->name, fname) == 0) {
                                        if (chunk_id >= 1 && chunk_id <= 4)
                                            curr->chunks[chunk_id-1] = 1;
                                        found = 1;
                                        break;
                                    }
                                    curr = curr->next;
                                }
                                if (!found) {
                                    struct FileInfo *new_node = malloc(sizeof(struct FileInfo));
                                    strcpy(new_node->name, fname);
                                    memset(new_node->chunks, 0, sizeof(new_node->chunks));
                                    if (chunk_id >= 1 && chunk_id <= 4)
                                        new_node->chunks[chunk_id-1] = 1;
                                    new_node->next = head;
                                    head = new_node;
                                }
                            }
                        }
                    }
                }
            }
            fclose(sock_fp);
        }
        close(sock);
    }

    struct FileInfo *curr = head;
    while (curr) {
        int complete = 1;
        for (int k=0; k<4; k++) if (!curr->chunks[k]) complete = 0;
        
        if (complete) {
            printf("%s\n", curr->name);
        } else {
            printf("%s [incomplete]\n", curr->name);
        }
        curr = curr->next;
    }
    // Free list... (omitted for brevity)
}

void get_file(const char *filename) {
    // Check which chunks we can get
    int chunks_found[4] = {0, 0, 0, 0};
    char *chunk_data[4] = {NULL, NULL, NULL, NULL};
    long chunk_lens[4] = {0, 0, 0, 0};

    for (int i = 0; i < num_servers; i++) {
        // Optimization: if we have all chunks, stop? 
        // No, we might need to try multiple servers if one fails?
        // But we can stop if we have all 4.
        if (chunks_found[0] && chunks_found[1] && chunks_found[2] && chunks_found[3]) break;

        // Try to get chunks from this server
        // We don't know which chunks this server has unless we calculate hash mod.
        // But the server might have them even if it's not the primary owner? No, only owners store.
        // But wait, if we are downloading, we should probably just ask for chunks we need.
        // We can calculate where chunks SHOULD be.
        
        int x = get_hash_mod(filename);
        // Server i should have:
        int c1 = distribution[x][i][0];
        int c2 = distribution[x][i][1];
        
        int chunks_to_fetch[2] = {c1, c2};
        
        for (int k=0; k<2; k++) {
            int c_idx = chunks_to_fetch[k] - 1;
            if (chunks_found[c_idx]) continue; // Already have it

            int sock = connect_to_server(i);
            if (sock < 0) continue;

            char chunk_name[300];
            snprintf(chunk_name, sizeof(chunk_name), ".%s.%d", filename, c_idx + 1);
            
            dprintf(sock, "GET %s\n", chunk_name);
            
            char buf[1024];
            ssize_t n = read(sock, buf, sizeof(buf));
            
            if (n > 0) {
                // Check for OK\n
                if (n >= 3 && strncmp(buf, "OK\n", 3) == 0) {
                    // Data starts at buf + 3
                    size_t data_len = n - 3;
                    size_t total_read = 0;
                    size_t capacity = 1024;
                    if (data_len > capacity) capacity = data_len * 2;
                    char *data = malloc(capacity);
                    
                    if (data_len > 0) {
                        memcpy(data, buf + 3, data_len);
                        total_read = data_len;
                    }
                    
                    ssize_t r;
                    while ((r = read(sock, buf, sizeof(buf))) > 0) {
                        if (total_read + r > capacity) {
                            capacity *= 2;
                            data = realloc(data, capacity);
                        }
                        memcpy(data + total_read, buf, r);
                        total_read += r;
                    }
                    
                    chunk_data[c_idx] = data;
                    chunk_lens[c_idx] = total_read;
                    chunks_found[c_idx] = 1;
                }
            }
            close(sock);
        }
    }

    if (chunks_found[0] && chunks_found[1] && chunks_found[2] && chunks_found[3]) {
        FILE *f = fopen(filename, "wb");
        if (f) {
            for (int k=0; k<4; k++) {
                fwrite(chunk_data[k], 1, chunk_lens[k], f);
                free(chunk_data[k]);
            }
            fclose(f);
            printf("Got %s\n", filename); // Not in spec but helpful? Spec says "write it to the current working directory"
        }
    } else {
        printf("%s is incomplete\n", filename);
        for (int k=0; k<4; k++) if(chunk_data[k]) free(chunk_data[k]);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [filename] ...\n", argv[0]);
        exit(1);
    }

    read_config();

    char *command = argv[1];

    if (strcmp(command, "list") == 0) {
        list_files();
    } else if (strcmp(command, "get") == 0) {
        for (int i = 2; i < argc; i++) {
            get_file(argv[i]);
        }
    } else if (strcmp(command, "put") == 0) {
        for (int i = 2; i < argc; i++) {
            put_file(argv[i]);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        exit(1);
    }

    return 0;
}
