#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#define BUFFER_SIZE 8192
#define MAX_BLOCKLIST 1000
#define CACHE_DIR "./cache"
#define BLOCKLIST_FILE "./blocklist"

// Global variables
int server_socket = -1;
int cache_timeout = 0;
char *blocklist[MAX_BLOCKLIST];
int blocklist_count = 0;
pthread_mutex_t blocklist_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int client_sock;
    struct sockaddr_in client_addr;
} client_info_t;

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down proxy server...\n");
    if (server_socket >= 0) {
        close(server_socket);
    }
    exit(0);
}

void load_blocklist() {
    FILE *fp = fopen(BLOCKLIST_FILE, "r");
    if (!fp) {
        return;
    }
    
    char line[512];
    pthread_mutex_lock(&blocklist_mutex);
    while (fgets(line, sizeof(line), fp) && blocklist_count < MAX_BLOCKLIST) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        if (strlen(line) > 0 && line[0] != '#') {
            blocklist[blocklist_count] = strdup(line);
            blocklist_count++;
        }
    }
    pthread_mutex_unlock(&blocklist_mutex);
    fclose(fp);
}

int is_blocked(const char *host) {
    pthread_mutex_lock(&blocklist_mutex);
    for (int i = 0; i < blocklist_count; i++) {
        if (strstr(host, blocklist[i]) != NULL) {
            pthread_mutex_unlock(&blocklist_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&blocklist_mutex);
    return 0;
}

void send_error(int sock, int code, const char *message) {
    char response[1024];
    snprintf(response, sizeof(response),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/html\r\n"
             "Connection: close\r\n\r\n"
             "<html><body><h1>%d %s</h1></body></html>\r\n",
             code, message, code, message);
    send(sock, response, strlen(response), 0);
}

void compute_hash(const char *url, char *hash_str) {
    unsigned long hash = 5381;
    const char *ptr = url;
    int c;
    
    while ((c = *ptr++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    snprintf(hash_str, 33, "%016lx", hash);
}

int is_dynamic_url(const char *url) {
    return strchr(url, '?') != NULL;
}

char* get_cache_path(const char *url) {
    char hash[33];
    compute_hash(url, hash);
    
    char *path = malloc(512);
    snprintf(path, 512, "%s/%s", CACHE_DIR, hash);
    return path;
}

int check_cache_valid(const char *cache_path) {
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        return 0;
    }
    
    if (cache_timeout <= 0) {
        return 1;
    }
    
    time_t now = time(NULL);
    time_t age = now - st.st_mtime;
    
    return age < cache_timeout;
}

int read_from_cache(const char *cache_path, int client_sock) {
    FILE *fp = fopen(cache_path, "rb");
    if (!fp) return 0;
    
    char buffer[BUFFER_SIZE];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        send(client_sock, buffer, bytes, 0);
    }
    
    fclose(fp);
    return 1;
}

void write_to_cache(const char *cache_path, const char *data, size_t len) {
    pthread_mutex_lock(&cache_mutex);
    
    FILE *fp = fopen(cache_path, "wb");
    if (fp) {
        fwrite(data, 1, len, fp);
        fclose(fp);
    }
    
    pthread_mutex_unlock(&cache_mutex);
}

int parse_url(const char *url, char *host, int *port, char *path) {
    char *url_copy = strdup(url);
    char *ptr = url_copy;
    
    if (strncmp(ptr, "http://", 7) == 0) {
        ptr += 7;
    }
    
    char *slash = strchr(ptr, '/');
    char *colon = strchr(ptr, ':');
    
    if (slash) {
        strcpy(path, slash);
        *slash = '\0';
    } else {
        strcpy(path, "/");
    }
    
    if (colon && (!slash || colon < slash)) {
        *colon = '\0';
        strcpy(host, ptr);
        *port = atoi(colon + 1);
    } else {
        strcpy(host, ptr);
        *port = 80;
    }
    
    free(url_copy);
    return 1;
}

void extract_links_and_prefetch(const char *base_url, const char *html_content, size_t content_len);

void *handle_client(void *arg) {
    client_info_t *client = (client_info_t *)arg;
    int client_sock = client->client_sock;
    char buffer[BUFFER_SIZE];
    
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_sock);
        free(client);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    
    char method[16], url[2048], version[16];
    if (sscanf(buffer, "%s %s %s", method, url, version) != 3) {
        send_error(client_sock, 400, "Bad Request");
        close(client_sock);
        free(client);
        return NULL;
    }
    
    if (strcmp(method, "GET") != 0) {
        send_error(client_sock, 400, "Bad Request");
        close(client_sock);
        free(client);
        return NULL;
    }
    
    char host[512], path[2048];
    int port;
    if (!parse_url(url, host, &port, path)) {
        send_error(client_sock, 400, "Bad Request");
        close(client_sock);
        free(client);
        return NULL;
    }
    
    if (is_blocked(host)) {
        send_error(client_sock, 403, "Forbidden");
        close(client_sock);
        free(client);
        return NULL;
    }
    
    struct hostent *server = gethostbyname(host);
    if (!server) {
        send_error(client_sock, 404, "Not Found");
        close(client_sock);
        free(client);
        return NULL;
    }
    
    char *cache_path = NULL;
    int use_cache = 0;
    if (!is_dynamic_url(url)) {
        cache_path = get_cache_path(url);
        if (check_cache_valid(cache_path)) {
            if (read_from_cache(cache_path, client_sock)) {
                close(client_sock);
                free(cache_path);
                free(client);
                return NULL;
            }
        }
        use_cache = 1;
    }
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        send_error(client_sock, 500, "Internal Server Error");
        close(client_sock);
        if (cache_path) free(cache_path);
        free(client);
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    
    if (connect(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        send_error(client_sock, 502, "Bad Gateway");
        close(server_sock);
        close(client_sock);
        if (cache_path) free(cache_path);
        free(client);
        return NULL;
    }
    
    char request[4096];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
    
    send(server_sock, request, strlen(request), 0);
    
    char *response_data = NULL;
    size_t response_size = 0;
    size_t response_capacity = 0;
    
    while (1) {
        bytes_read = recv(server_sock, buffer, sizeof(buffer), 0);
        if (bytes_read <= 0) break;
        
        send(client_sock, buffer, bytes_read, 0);
        
        if (use_cache) {
            if (response_size + bytes_read > response_capacity) {
                response_capacity = (response_capacity == 0) ? BUFFER_SIZE : response_capacity * 2;
                char *new_data = realloc(response_data, response_capacity);
                if (!new_data) {
                    free(response_data);
                    response_data = NULL;
                    use_cache = 0;
                    continue;
                }
                response_data = new_data;
            }
            memcpy(response_data + response_size, buffer, bytes_read);
            response_size += bytes_read;
        }
    }
    
    if (use_cache && response_data && response_size > 0) {
        write_to_cache(cache_path, response_data, response_size);
        extract_links_and_prefetch(url, response_data, response_size);
    }
    
    if (response_data) free(response_data);
    if (cache_path) free(cache_path);
    close(server_sock);
    close(client_sock);
    free(client);
    
    return NULL;
}

void *prefetch_url(void *arg) {
    char *url = (char *)arg;
    
    char host[512], path[2048];
    int port;
    
    if (!parse_url(url, host, &port, path)) {
        free(url);
        return NULL;
    }
    
    if (is_blocked(host)) {
        free(url);
        return NULL;
    }
    
    if (is_dynamic_url(url)) {
        free(url);
        return NULL;
    }
    
    char *cache_path = get_cache_path(url);
    if (check_cache_valid(cache_path)) {
        free(cache_path);
        free(url);
        return NULL;
    }
    
    struct hostent *server = gethostbyname(host);
    if (!server) {
        free(cache_path);
        free(url);
        return NULL;
    }
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        free(cache_path);
        free(url);
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    if (connect(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_sock);
        free(cache_path);
        free(url);
        return NULL;
    }
    
    char request[4096];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
    
    send(server_sock, request, strlen(request), 0);
    
    char *response_data = NULL;
    size_t response_size = 0;
    size_t response_capacity = 0;
    char buffer[BUFFER_SIZE];
    
    while (1) {
        ssize_t bytes = recv(server_sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        
        if (response_size + bytes > response_capacity) {
            response_capacity = (response_capacity == 0) ? BUFFER_SIZE : response_capacity * 2;
            char *new_data = realloc(response_data, response_capacity);
            if (!new_data) {
                if (response_data) free(response_data);
                free(cache_path);
                close(server_sock);
                free(url);
                return NULL;
            }
            response_data = new_data;
        }
        memcpy(response_data + response_size, buffer, bytes);
        response_size += bytes;
    }
    
    if (response_data && response_size > 0) {
        write_to_cache(cache_path, response_data, response_size);
    }
    
    if (response_data) free(response_data);
    free(cache_path);
    close(server_sock);
    free(url);
    
    return NULL;
}

void extract_links_and_prefetch(const char *base_url, const char *html_content, size_t content_len) {
    char *content_str = malloc(content_len + 1);
    memcpy(content_str, html_content, content_len);
    content_str[content_len] = '\0';
    
    char *body = strstr(content_str, "\r\n\r\n");
    if (body) {
        body += 4;
    } else {
        body = strstr(content_str, "\n\n");
        if (body) {
            body += 2;
        } else {
            body = content_str;
        }
    }
    
    if (!strstr(body, "<html") && !strstr(body, "<HTML") && 
        !strstr(body, "<!DOCTYPE") && !strstr(body, "<!doctype")) {
        free(content_str);
        return;
    }
    
    char base_host[512];
    int base_port;
    char base_path[2048];
    parse_url(base_url, base_host, &base_port, &base_path);
    
    char *ptr = body;
    int prefetch_count = 0;
    
    while (ptr && prefetch_count < 20) {
        char *href_dq = strstr(ptr, "href=\"");
        char *href_sq = strstr(ptr, "href='");
        char *src_dq = strstr(ptr, "src=\"");
        char *src_sq = strstr(ptr, "src='");
        
        char *link_start = NULL;
        char quote_char = '"';
        char *earliest = NULL;
        
        if (href_dq) earliest = href_dq;
        if (href_sq && (!earliest || href_sq < earliest)) earliest = href_sq;
        if (src_dq && (!earliest || src_dq < earliest)) earliest = src_dq;
        if (src_sq && (!earliest || src_sq < earliest)) earliest = src_sq;
        
        if (!earliest) break;
        
        if (earliest == href_dq) {
            link_start = earliest + 6;
            quote_char = '"';
        } else if (earliest == href_sq) {
            link_start = earliest + 6;
            quote_char = '\'';
        } else if (earliest == src_dq) {
            link_start = earliest + 5;
            quote_char = '"';
        } else {
            link_start = earliest + 5;
            quote_char = '\'';
        }
        
        ptr = link_start;
        char *link_end = strchr(link_start, quote_char);
        if (!link_end) break;
        
        size_t link_len = link_end - link_start;
        if (link_len == 0 || link_len > 1024) {
            ptr = link_end + 1;
            continue;
        }
        
        char link[1024];
        strncpy(link, link_start, link_len);
        link[link_len] = '\0';
        
        if (strncmp(link, "javascript:", 11) == 0 ||
            strncmp(link, "mailto:", 7) == 0 ||
            strncmp(link, "https:", 6) == 0 ||
            strncmp(link, "#", 1) == 0) {
            ptr = link_end + 1;
            continue;
        }
        
        char full_url[2048];
        if (strncmp(link, "http://", 7) == 0) {
            strncpy(full_url, link, sizeof(full_url) - 1);
            full_url[sizeof(full_url) - 1] = '\0';
        } else if (link[0] == '/') {
            snprintf(full_url, sizeof(full_url), "http://%s%s", base_host, link);
        } else {
            char *last_slash = strrchr(base_path, '/');
            if (last_slash) {
                char dir[2048];
                size_t dir_len = last_slash - base_path + 1;
                strncpy(dir, base_path, dir_len);
                dir[dir_len] = '\0';
                snprintf(full_url, sizeof(full_url), "http://%s%s%s", base_host, dir, link);
            } else {
                snprintf(full_url, sizeof(full_url), "http://%s/%s", base_host, link);
            }
        }
        
        pthread_t tid;
        char *url_copy = strdup(full_url);
        pthread_create(&tid, NULL, prefetch_url, url_copy);
        pthread_detach(tid);
        
        prefetch_count++;
        ptr = link_end + 1;
    }
    
    free(content_str);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [timeout]\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    if (argc >= 3) {
        cache_timeout = atoi(argv[2]);
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    mkdir(CACHE_DIR, 0755);
    load_blocklist();
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return 1;
    }
    
    if (listen(server_socket, 50) < 0) {
        perror("listen");
        close(server_socket);
        return 1;
    }
    
    printf("Proxy server listening on port %d (timeout: %d seconds)\n", port, cache_timeout);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (errno == EINTR) {
                break;
            }
            continue;
        }
        
        client_info_t *client = malloc(sizeof(client_info_t));
        client->client_sock = client_sock;
        client->client_addr = client_addr;
        
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, client);
        pthread_detach(tid);
    }
    
    close(server_socket);
    return 0;
}