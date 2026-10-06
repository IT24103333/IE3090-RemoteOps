#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define DEFAULT_PORT 9410
#define MAX_LINE 4096

int tcp_fd = -1;
volatile int monitor_running = 0;

ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return n;
        sent += n;
    }
    return sent;
}

ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    char *p = buf;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return n;
        got += n;
    }
    return got;
}

ssize_t read_line(int fd, char *buf, size_t max) {
    size_t pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') {
            buf[pos] = '\0';
            return pos;
        }
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

void *udp_listener(void *arg) {
    int port = *(int *)arg;
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr));

    char buf[512];
    while (monitor_running) {
        ssize_t n = recvfrom(udp_fd, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';
            printf("\n[UDP MONITOR] %s\n> ", buf);
            fflush(stdout);
        }
    }
    close(udp_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to RemoteOps Agent on port %d\n", port);
    printf("Type 'help' for commands\n");

    char line[MAX_LINE], resp[MAX_LINE];

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 0) continue;

        if (strcmp(line, "help") == 0) {
            printf("AUTH OPS-3333\nSYSINFO\nLISTPROC\nEXEC DATE|UPTIME|DISKFREE|HOSTNAME|WHOAMI\n");
            printf("PUT <local> <remote>\nGET <remote> <local>\nMONITOR START <port>\nMONITOR STOP\nQUIT\n");
            continue;
        }

        if (strncmp(line, "PUT ", 4) == 0) {
            char local[256], remote[256];
            if (sscanf(line+4, "%255s %255s", local, remote) != 2) {
                printf("Usage: PUT <localfile> <remotename>\n");
                continue;
            }
            FILE *fp = fopen(local, "rb");
            if (!fp) { perror("fopen"); continue; }
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "PUT %s %ld\n", remote, size);
            send_all(tcp_fd, cmd, strlen(cmd));
            char *buf = malloc(size);
            fread(buf, 1, size, fp);
            send_all(tcp_fd, buf, size);
            free(buf);
            fclose(fp);
            if (read_line(tcp_fd, resp, sizeof(resp)) > 0) printf("%s\n", resp);
            continue;
        }

        if (strncmp(line, "GET ", 4) == 0) {
            char remote[256], local[256];
            if (sscanf(line+4, "%255s %255s", remote, local) != 2) {
                printf("Usage: GET <remotename> <localfile>\n");
                continue;
            }
            char cmd[300];
            snprintf(cmd, sizeof(cmd), "GET %s\n", remote);
            send_all(tcp_fd, cmd, strlen(cmd));
            if (read_line(tcp_fd, resp, sizeof(resp)) <= 0) continue;
            printf("%s\n", resp);
            if (strncmp(resp, "OK FILE_SEND ", 13) == 0) {
                char fname[256];
                long fsize = 0;
                sscanf(resp+13, "%255s %ld", fname, &fsize);
                char *buf = malloc(fsize);
                if (recv_all(tcp_fd, buf, fsize) == fsize) {
                    FILE *fp = fopen(local, "wb");
                    if (fp) {
                        fwrite(buf, 1, fsize, fp);
                        fclose(fp);
                        printf("Saved %ld bytes to %s\n", fsize, local);
                    }
                }
                free(buf);
            }
            continue;
        }

        if (strncmp(line, "MONITOR START ", 14) == 0) {
            int udp_port = atoi(line+14);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "MONITOR START %d\n", udp_port);
            send_all(tcp_fd, cmd, strlen(cmd));
            if (read_line(tcp_fd, resp, sizeof(resp)) > 0) printf("%s\n", resp);
            if (strncmp(resp, "OK MONITOR_STARTED", 18) == 0) {
                monitor_running = 1;
                pthread_t tid;
                int *p = malloc(sizeof(int));
                *p = udp_port;
                pthread_create(&tid, NULL, udp_listener, p);
                pthread_detach(tid);
            }
            continue;
        }

        if (strcmp(line, "MONITOR STOP") == 0) {
            send_all(tcp_fd, "MONITOR STOP\n", 13);
            if (read_line(tcp_fd, resp, sizeof(resp)) > 0) printf("%s\n", resp);
            monitor_running = 0;
            continue;
        }

        char cmd[MAX_LINE];
        snprintf(cmd, sizeof(cmd), "%s\n", line);
        send_all(tcp_fd, cmd, strlen(cmd));
        if (read_line(tcp_fd, resp, sizeof(resp)) > 0) printf("%s\n", resp);
        if (strcmp(line, "QUIT") == 0) break;
    }
    monitor_running = 0;
    close(tcp_fd);
    return 0;
}
