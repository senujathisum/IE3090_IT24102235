/*
 * controller_235.c - RemoteOps Controller (TCP client + UDP monitor receiver)
 *
 * IE3090 Network Programming - Assignment (Part 1)
 * Registration number : IT24102235
 *
 * Personalised values (derived from IT24102235, see assignment section 2.4):
 *   Agent port   : 7000 + 2410 = 9410
 *   SID tag      : SID:5322      Auth token : OPS-2235
 *
 * Build : make -f Makefile_235
 * Run   : ./controller_235 [agent_ip] [agent_port]
 *         (defaults: 127.0.0.1 9410)
 *
 * Commands typed by the user are sent to the Agent exactly as typed, except:
 *   PUT <local_file>       -> controller sends "PUT <name> <size>" + file bytes
 *   GET <filename>         -> controller saves the bytes in ./downloads/<filename>
 *   MONITOR START <port>   -> controller first opens a UDP socket on <port>
 *   HELP                   -> local help, nothing is sent
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_IP       "127.0.0.1"
#define DEFAULT_PORT     9410                 /* 7000 + 2410 */
#define DOWNLOAD_DIR     "./downloads"
#define FILE_CHUNK       4096
#define MAX_INPUT        1024
#define MAX_RESP_LINE    70000                /* LISTPROC lines can be long */

/* Receive buffer for the TCP connection (same idea as in the Agent):
 * keeps bytes that were received but not used yet. */
typedef struct {
    int  fd;
    char buf[MAX_RESP_LINE];
    int  len;
} conn_reader;

static conn_reader reader;

/* UDP monitoring receiver */
static int             udp_sock = -1;
static int             udp_port_open = 0;
static int             udp_running = 0;      /* protected by udp_lock */
static pthread_mutex_t udp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       udp_tid;

/* ================================================================== */
/*                  Sending / receiving helpers (framing)             */
/* ================================================================== */

static int send_all(int sock, const char *data, long len)
{
    long total = 0;

    while (total < len) {
        ssize_t n = send(sock, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

/* Read one '\n'-terminated response line. Returns its length, or -1 if
 * the Agent closed the connection, -2 if the line is too long. */
static int read_line(conn_reader *r, char *line, int max)
{
    while (1) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            int line_len = (int)(nl - r->buf);
            int used     = line_len + 1;

            if (line_len >= max)
                return -2;
            memcpy(line, r->buf, line_len);
            line[line_len] = '\0';
            memmove(r->buf, r->buf + used, r->len - used);
            r->len -= used;
            return line_len;
        }
        if (r->len == (int)sizeof(r->buf))
            return -2;

        ssize_t n = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        r->len += (int)n;
    }
}

/* Receive exactly 'size' bytes (after "OK FILE_SEND ...") into fp.
 * Leftover bytes in the reader buffer are used first. */
static int receive_file_data(conn_reader *r, FILE *fp, long size)
{
    char chunk[FILE_CHUNK];
    long remaining = size;

    while (remaining > 0) {
        long want = remaining < FILE_CHUNK ? remaining : FILE_CHUNK;
        long got;

        if (r->len > 0) {
            got = r->len < want ? r->len : want;
            memcpy(chunk, r->buf, got);
            memmove(r->buf, r->buf + got, r->len - got);
            r->len -= (int)got;
        } else {
            ssize_t n = recv(r->fd, chunk, want, 0);
            if (n == 0)
                return -1;
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            got = n;
        }
        if (fwrite(chunk, 1, got, fp) != (size_t)got)
            return -2;
        remaining -= got;
    }
    return 0;
}

static double seconds_since(struct timeval *start)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (now.tv_sec - start->tv_sec) + (now.tv_usec - start->tv_usec) / 1e6;
}

/* Read one response line and print it. Returns 0 if the connection was
 * lost, otherwise 1. The line is also copied into 'out' when given. */
static int print_response(char *out, int out_size)
{
    static char line[MAX_RESP_LINE];
    int n = read_line(&reader, line, sizeof(line));

    if (n == -1) {
        printf("!! Agent closed the connection.\n");
        return 0;
    }
    if (n == -2) {
        printf("!! Response line too long - closing.\n");
        return 0;
    }
    printf("<< %s\n", line);
    if (out != NULL) {
        int copy_len = (n < out_size - 1) ? n : out_size - 1;
        memcpy(out, line, copy_len);
        out[copy_len] = '\0';
    }
    return 1;
}

/* ================================================================== */
/*                 UDP monitoring receiver (thread)                   */
/* ================================================================== */

static int udp_is_running(void)
{
    int running;

    pthread_mutex_lock(&udp_lock);
    running = udp_running;
    pthread_mutex_unlock(&udp_lock);
    return running;
}

/* Wait for datagrams with select() (1 s timeout so the thread can notice
 * when it should stop) and print each one. */
static void *udp_receiver_thread(void *arg)
{
    (void)arg;
    char buf[512];

    while (udp_is_running()) {
        fd_set         fds;
        struct timeval tv;

        FD_ZERO(&fds);
        FD_SET(udp_sock, &fds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        if (select(udp_sock + 1, &fds, NULL, NULL, &tv) > 0) {
            struct sockaddr_in from;
            socklen_t          from_len = sizeof(from);
            ssize_t n = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&from, &from_len);
            if (n > 0) {
                char ip[INET_ADDRSTRLEN];
                buf[n] = '\0';
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                printf("\n[UDP MONITOR from %s:%d] %s\nremoteops> ",
                       ip, ntohs(from.sin_port), buf);
                fflush(stdout);
            }
        }
    }
    return NULL;
}

/* Open the UDP socket on 'port' and start the receiver thread. */
static int start_udp_receiver(int port)
{
    struct sockaddr_in addr;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("UDP socket failed");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("UDP bind failed (port in use?)");
        close(udp_sock);
        udp_sock = -1;
        return -1;
    }

    pthread_mutex_lock(&udp_lock);
    udp_running = 1;
    pthread_mutex_unlock(&udp_lock);
    if (pthread_create(&udp_tid, NULL, udp_receiver_thread, NULL) != 0) {
        udp_running = 0;
        close(udp_sock);
        udp_sock = -1;
        return -1;
    }
    udp_port_open = port;
    printf("   (listening for UDP monitor datagrams on port %d)\n", port);
    return 0;
}

static void stop_udp_receiver(void)
{
    if (!udp_is_running())
        return;
    pthread_mutex_lock(&udp_lock);
    udp_running = 0;
    pthread_mutex_unlock(&udp_lock);
    pthread_join(udp_tid, NULL);
    close(udp_sock);
    udp_sock = -1;
    udp_port_open = 0;
}

/* ================================================================== */
/*                  Commands that need extra work                     */
/* ================================================================== */

/* PUT <local_file>: send "PUT <name> <size>\n" then exactly <size> bytes. */
static int do_put(int sock, const char *local_path)
{
    struct stat    st;
    char           header[512];
    char           chunk[FILE_CHUNK];
    char           resp[512];
    const char    *name;
    FILE          *fp;
    size_t         n;
    long           sent = 0;
    struct timeval start;

    if (stat(local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("!! Local file '%s' not found.\n", local_path);
        return 1;
    }
    fp = fopen(local_path, "rb");
    if (fp == NULL) {
        perror("!! Cannot open local file");
        return 1;
    }

    name = strrchr(local_path, '/');              /* send only the base name */
    name = (name != NULL) ? name + 1 : local_path;

    snprintf(header, sizeof(header), "PUT %s %ld\n", name, (long)st.st_size);
    printf(">> PUT %s %ld   (+ %ld raw bytes)\n", name, (long)st.st_size, (long)st.st_size);

    gettimeofday(&start, NULL);
    if (send_all(sock, header, strlen(header)) < 0) {
        fclose(fp);
        return 0;
    }
    while (sent < (long)st.st_size && (n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if ((long)n > st.st_size - sent)
            n = st.st_size - sent;
        if (send_all(sock, chunk, n) < 0) {
            fclose(fp);
            return 0;
        }
        sent += n;
    }
    fclose(fp);

    if (!print_response(resp, sizeof(resp)))
        return 0;
    if (strncmp(resp, "OK FILE_RECEIVED", 16) == 0) {
        double secs = seconds_since(&start);
        printf("   Uploaded %ld bytes in %.3f s  (throughput: %.0f bytes/s)\n",
               sent, secs, secs > 0 ? sent / secs : (double)sent);
    }
    return 1;
}

/* GET <filename>: read "OK FILE_SEND <name> <size> SID:..." then exactly
 * <size> bytes, saved as ./downloads/<name>. */
static int do_get(int sock, const char *line)
{
    char           resp[512];
    char           name[256];
    char           path[400];
    long           size;
    FILE          *fp;
    int            rc;
    struct timeval start;

    printf(">> %s\n", line);
    gettimeofday(&start, NULL);
    if (send_all(sock, line, strlen(line)) < 0 || send_all(sock, "\n", 1) < 0)
        return 0;
    if (!print_response(resp, sizeof(resp)))
        return 0;
    if (sscanf(resp, "OK FILE_SEND %255s %ld", name, &size) != 2)
        return 1;                                 /* an ERR line - already printed */

    /* never let the Agent's reply choose a path outside DOWNLOAD_DIR */
    if (strchr(name, '/') != NULL || name[0] == '.')
        snprintf(name, sizeof(name), "downloaded_file");

    mkdir(DOWNLOAD_DIR, 0755);
    snprintf(path, sizeof(path), "%s/%s", DOWNLOAD_DIR, name);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("!! Cannot create download file");
        return 0;                                 /* stream would be out of sync */
    }
    rc = receive_file_data(&reader, fp, size);
    fclose(fp);
    if (rc != 0) {
        printf("!! Download failed - connection lost.\n");
        remove(path);
        return 0;
    }

    double secs = seconds_since(&start);
    printf("   Saved %ld bytes to %s in %.3f s  (throughput: %.0f bytes/s)\n",
           size, path, secs, secs > 0 ? size / secs : (double)size);
    return 1;
}

/* MONITOR START <port>: open the UDP port BEFORE asking the Agent to
 * start, so the first datagram is not lost. */
static int do_monitor_start(int sock, const char *line, const char *port_text)
{
    char resp[512];
    int  port = atoi(port_text);
    int  opened_now = 0;

    if (port >= 1 && port <= 65535 && udp_port_open != port) {
        stop_udp_receiver();
        if (start_udp_receiver(port) < 0)
            return 1;
        opened_now = 1;
    }

    printf(">> %s\n", line);
    if (send_all(sock, line, strlen(line)) < 0 || send_all(sock, "\n", 1) < 0)
        return 0;
    if (!print_response(resp, sizeof(resp)))
        return 0;
    if (strncmp(resp, "OK MONITOR_STARTED", 18) != 0 && opened_now)
        stop_udp_receiver();                      /* Agent refused */
    return 1;
}

static void print_help(void)
{
    printf("Commands:\n"
           "  AUTH <token>             authenticate (must be first)\n"
           "  SYSINFO                  CPU load, memory used (MB), uptime (s)\n"
           "  LISTPROC                 list running processes\n"
           "  EXEC <name>              DATE | UPTIME | DISKFREE | HOSTNAME | WHOAMI\n"
           "  PUT <local_file>         upload a file to the Agent\n"
           "  GET <filename>           download a file into %s/\n"
           "  MONITOR START <udp_port> start periodic UDP stats\n"
           "  MONITOR STOP             stop periodic UDP stats\n"
           "  QUIT                     close the session\n"
           "  HELP                     show this help (local only)\n", DOWNLOAD_DIR);
}

/* ================================================================== */
/*                               main                                 */
/* ================================================================== */

int main(int argc, char **argv)
{
    const char        *ip   = (argc > 1) ? argv[1] : DEFAULT_IP;
    int                port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int                sock;
    struct sockaddr_in agent_addr;
    char               input[MAX_INPUT];
    int                at_eof = 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    memset(&agent_addr, 0, sizeof(agent_addr));
    agent_addr.sin_family = AF_INET;
    agent_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &agent_addr.sin_addr) <= 0) {
        printf("Invalid address: %s\n", ip);
        close(sock);
        exit(EXIT_FAILURE);
    }
    if (connect(sock, (struct sockaddr *)&agent_addr, sizeof(agent_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    reader.fd  = sock;
    reader.len = 0;
    printf("Connected to RemoteOps Agent at %s:%d\n", ip, port);
    print_help();

    while (1) {
        char  copy[MAX_INPUT];
        char *tok[4] = {NULL, NULL, NULL, NULL};
        char *save = NULL;
        int   ntok = 0;
        int   ok = 1;

        printf("remoteops> ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL) {    /* Ctrl+D / end of script */
            strcpy(input, "QUIT");
            at_eof = 1;
            printf("QUIT\n");
        }
        input[strcspn(input, "\r\n")] = '\0';
        if (input[0] == '\0')
            continue;

        snprintf(copy, sizeof(copy), "%s", input);
        for (char *t = strtok_r(copy, " \t", &save); t != NULL && ntok < 4;
             t = strtok_r(NULL, " \t", &save))
            tok[ntok++] = t;
        if (ntok == 0)
            continue;

        if (strcmp(tok[0], "HELP") == 0) {
            print_help();
            continue;
        } else if (strcmp(tok[0], "PUT") == 0) {
            if (ntok != 2) {
                printf("Usage: PUT <local_file>\n");
                continue;
            }
            ok = do_put(sock, tok[1]);
        } else if (strcmp(tok[0], "GET") == 0) {
            ok = do_get(sock, input);
        } else if (strcmp(tok[0], "MONITOR") == 0 && ntok == 3 &&
                   strcmp(tok[1], "START") == 0) {
            ok = do_monitor_start(sock, input, tok[2]);
        } else {
            /* everything else: send the line exactly as typed */
            char resp[512] = "";

            printf(">> %s\n", input);
            if (send_all(sock, input, strlen(input)) < 0 || send_all(sock, "\n", 1) < 0)
                ok = 0;
            else
                ok = print_response(resp, sizeof(resp));

            if (ok && strncmp(resp, "OK MONITOR_STOPPED", 18) == 0)
                stop_udp_receiver();
            if (ok && strncmp(resp, "OK BYE", 6) == 0)
                break;
        }

        if (!ok || at_eof)
            break;
    }

    stop_udp_receiver();
    close(sock);
    printf("Disconnected.\n");
    return 0;
}
