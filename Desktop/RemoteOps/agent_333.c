#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>

#define PORT            9410
#define SID_TAG         "SID:3333"
#define AUTH_TOKEN      "OPS-3333"
#define LOG_FILE        "remoteops_IT24103333.log"
#define STORAGE_DIR     "./agentfiles/IT24103333"
#define BACKLOG         10
#define MAX_LINE        4096
#define MAX_FILE_SIZE   (50 * 1024 * 1024)
#define MONITOR_INTERVAL 2

typedef struct {
    int tcp_fd;
    struct sockaddr_in client_addr;
    int authenticated;
    int monitor_active;
    int udp_port;
    pthread_t monitor_tid;
    int running;
} client_session_t;

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_msg(const char *msg) {
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    pthread_mutex_lock(&log_mutex);
    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        fprintf(fp, "[%s] %s\n", tbuf, msg);
        fclose(fp);
    }
    pthread_mutex_unlock(&log_mutex);
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

void get_sysinfo_str(char *out, size_t outlen) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        double load = si.loads[0] / 65536.0;
        unsigned long mem_used_mb = (si.totalram - si.freeram) / (1024 * 1024);
        snprintf(out, outlen, "%.2f %lu %ld", load, mem_used_mb, si.uptime);
    } else {
        snprintf(out, outlen, "0.50 512 3600");
    }
}

void get_process_list(char *out, size_t outlen) {
    FILE *fp = popen("ps -eo comm,pid --no-headers 2>/dev/null | head -n 30", "r");
    if (!fp) {
        snprintf(out, outlen, "unknown");
        return;
    }
    out[0] = '\0';
    char line[128];
    int first = 1;
    while (fgets(line, sizeof(line), fp) && strlen(out) < outlen - 64) {
        line[strcspn(line, "\n")] = 0;
        if (!first) strcat(out, ",");
        strcat(out, line);
        first = 0;
    }
    pclose(fp);
    if (out[0] == '\0') strcpy(out, "none");
}

int run_whitelisted(const char *name, char *out, size_t outlen) {
    const char *cmd = NULL;
    if      (strcmp(name, "DATE")     == 0) cmd = "date";
    else if (strcmp(name, "UPTIME")   == 0) cmd = "uptime -p 2>/dev/null || uptime";
    else if (strcmp(name, "DISKFREE") == 0) cmd = "df -h / | tail -1";
    else if (strcmp(name, "HOSTNAME")== 0) cmd = "hostname";
    else if (strcmp(name, "WHOAMI")  == 0) cmd = "whoami";
    else return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(out, outlen, "exec_failed");
        return 0;
    }
    size_t n = fread(out, 1, outlen - 1, fp);
    out[n] = '\0';
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    pclose(fp);
    return 0;
}

void *monitor_thread(void *arg) {
    client_session_t *sess = (client_session_t *)arg;
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) return NULL;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(sess->udp_port);
    dest.sin_addr   = sess->client_addr.sin_addr;

    char payload[256];
    while (sess->monitor_active && sess->running) {
        char stats[128];
        get_sysinfo_str(stats, sizeof(stats));
        snprintf(payload, sizeof(payload), "SYSINFO %s %s", stats, SID_TAG);
        sendto(udp_fd, payload, strlen(payload), 0, (struct sockaddr *)&dest, sizeof(dest));
        sleep(MONITOR_INTERVAL);
    }
    close(udp_fd);
    return NULL;
}

void send_response(int fd, const char *msg) {
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "%s %s\n", msg, SID_TAG);
    send_all(fd, line, strlen(line));
}

void handle_client(client_session_t *sess) {
    int fd = sess->tcp_fd;
    char line[MAX_LINE];
    char logbuf[MAX_LINE + 64];

    snprintf(logbuf, sizeof(logbuf), "NEW_CONNECTION from %s:%d",
             inet_ntoa(sess->client_addr.sin_addr), ntohs(sess->client_addr.sin_port));
    log_msg(logbuf);

    while (sess->running) {
        ssize_t n = read_line(fd, line, sizeof(line));
        if (n <= 0) {
            log_msg("CLIENT_DISCONNECTED");
            break;
        }

        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = 0;
        snprintf(logbuf, sizeof(logbuf), "CMD: %.4000s", line);
        log_msg(logbuf);

        if (strncmp(line, "AUTH ", 5) == 0) {
            if (strcmp(line + 5, AUTH_TOKEN) == 0) {
                sess->authenticated = 1;
                send_response(fd, "OK AUTHENTICATED");
            } else {
                send_response(fd, "ERR 001 AUTH_FAILED");
            }
            continue;
        }

        if (!sess->authenticated) {
            send_response(fd, "ERR 001 AUTH_REQUIRED");
            continue;
        }

        if (strcmp(line, "SYSINFO") == 0) {
            char stats[128];
            get_sysinfo_str(stats, sizeof(stats));
            char resp[256];
            snprintf(resp, sizeof(resp), "OK SYSINFO %s", stats);
            send_response(fd, resp);
            continue;
        }

        if (strcmp(line, "LISTPROC") == 0) {
            char procs[2048];
            get_process_list(procs, sizeof(procs));
            char resp[2200];
            snprintf(resp, sizeof(resp), "OK PROCS %s", procs);
            send_response(fd, resp);
            continue;
        }

        if (strncmp(line, "EXEC ", 5) == 0) {
            char output[1024];
            if (run_whitelisted(line + 5, output, sizeof(output)) == 0) {
                char resp[1200];
                snprintf(resp, sizeof(resp), "OK EXEC_RESULT %s", output);
                send_response(fd, resp);
            } else {
                send_response(fd, "ERR 002 COMMAND_NOT_ALLOWED");
            }
            continue;
        }

        if (strncmp(line, "PUT ", 4) == 0) {
            char filename[256];
            long filesize = 0;
            if (sscanf(line + 4, "%255s %ld", filename, &filesize) != 2 || filesize < 0 || filesize > MAX_FILE_SIZE) {
                send_response(fd, "ERR 004 FILE_TOO_LARGE");
                continue;
            }
            char *base = strrchr(filename, '/');
            if (base) base++; else base = filename;
            if (strstr(base, "..")) {
                send_response(fd, "ERR 004 FILE_TOO_LARGE");
                continue;
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, base);
            char *filebuf = malloc(filesize);
            if (!filebuf || recv_all(fd, filebuf, filesize) != filesize) {
                free(filebuf);
                send_response(fd, "ERR 004 FILE_TOO_LARGE");
                continue;
            }
            FILE *fp = fopen(path, "wb");
            if (!fp) {
                free(filebuf);
                send_response(fd, "ERR 004 FILE_TOO_LARGE");
                continue;
            }
            fwrite(filebuf, 1, filesize, fp);
            fclose(fp);
            free(filebuf);
            char resp[300];
            snprintf(resp, sizeof(resp), "OK FILE_RECEIVED %s", base);
            send_response(fd, resp);
            snprintf(logbuf, sizeof(logbuf), "PUT %s (%ld bytes)", base, filesize);
            log_msg(logbuf);
            continue;
        }

        if (strncmp(line, "GET ", 4) == 0) {
            char filename[256];
            if (sscanf(line + 4, "%255s", filename) != 1) {
                send_response(fd, "ERR 005 FILE_NOT_FOUND");
                continue;
            }
            char *base = strrchr(filename, '/');
            if (base) base++; else base = filename;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, base);
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                send_response(fd, "ERR 005 FILE_NOT_FOUND");
                continue;
            }
            fseek(fp, 0, SEEK_END);
            long filesize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char header[300];
            snprintf(header, sizeof(header), "OK FILE_SEND %s %ld %s\n", base, filesize, SID_TAG);
            send_all(fd, header, strlen(header));
            char *filebuf = malloc(filesize);
            if (filebuf) {
                fread(filebuf, 1, filesize, fp);
                send_all(fd, filebuf, filesize);
                free(filebuf);
            }
            fclose(fp);
            snprintf(logbuf, sizeof(logbuf), "GET %s (%ld bytes)", base, filesize);
            log_msg(logbuf);
            continue;
        }

        if (strncmp(line, "MONITOR START ", 14) == 0) {
            int udp_port = atoi(line + 14);
            if (udp_port <= 0 || udp_port > 65535) {
                send_response(fd, "ERR 003 INVALID_PORT");
                continue;
            }
            if (sess->monitor_active) {
                sess->monitor_active = 0;
                pthread_join(sess->monitor_tid, NULL);
            }
            sess->udp_port = udp_port;
            sess->monitor_active = 1;
            pthread_create(&sess->monitor_tid, NULL, monitor_thread, sess);
            send_response(fd, "OK MONITOR_STARTED");
            continue;
        }

        if (strcmp(line, "MONITOR STOP") == 0) {
            if (sess->monitor_active) {
                sess->monitor_active = 0;
                pthread_join(sess->monitor_tid, NULL);
            }
            send_response(fd, "OK MONITOR_STOPPED");
            continue;
        }

        if (strcmp(line, "QUIT") == 0) {
            if (sess->monitor_active) {
                sess->monitor_active = 0;
                pthread_join(sess->monitor_tid, NULL);
            }
            send_response(fd, "OK BYE");
            break;
        }

        send_response(fd, "ERR 003 UNKNOWN_COMMAND");
    }

    if (sess->monitor_active) {
        sess->monitor_active = 0;
        pthread_join(sess->monitor_tid, NULL);
    }
    close(fd);
    sess->running = 0;
    log_msg("SESSION_CLOSED");
}

void *client_thread(void *arg) {
    client_session_t *sess = (client_session_t *)arg;
    handle_client(sess);
    free(sess);
    return NULL;
}

int main(void) {
    mkdir("./agentfiles", 0755);
    mkdir(STORAGE_DIR, 0755);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    printf("RemoteOps Agent (IT24103333) listening on port %d  SID:3333\n", PORT);
    log_msg("AGENT_STARTED");

    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int confd = accept(listen_fd, (struct sockaddr *)&cli, &clilen);
        if (confd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        client_session_t *sess = calloc(1, sizeof(*sess));
        sess->tcp_fd = confd;
        sess->client_addr = cli;
        sess->running = 1;

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, sess);
        pthread_detach(tid);
    }
    close(listen_fd);
    return 0;
}
