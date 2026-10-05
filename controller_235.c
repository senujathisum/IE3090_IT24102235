#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_IP    "127.0.0.1"
#define DEFAULT_PORT  9410
#define MAX_LINE      1024

int main(int argc, char **argv)
{
    const char *ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Connect failed");
        return 1;
    }
    printf("Connected to RemoteOps Agent at %s:%d\n", ip, port);

    char input[MAX_LINE];
    char resp[MAX_LINE];
    while (1) {
        printf("remoteops> ");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\r\n")] = '\0';
        if (input[0] == '\0') continue;

        strcat(input, "\n");
        send(sock, input, strlen(input), 0);
        ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
        if (n <= 0) break;
        resp[n] = '\0';
        printf("<< %s", resp);
        if (strncmp(resp, "OK BYE", 6) == 0) break;
    }
    close(sock);
    return 0;
}