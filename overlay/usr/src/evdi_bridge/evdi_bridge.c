#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

// Protocol definitions from Anland (display_protocol.h)
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_PICKUP_FDS      9
#define DATA_MSG_BUF_READY       100

struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct screen_info {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t refresh;
} __attribute__((packed));

int recv_fds(int sock, void *data, size_t data_len, int *fds, int fd_count, int *fds_received) {
    struct iovec iov = { .iov_base = data, .iov_len  = data_len };
    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf) };

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    *fds_received = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (count > fd_count) count = fd_count;
        memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * count);
        *fds_received = count;
    }
    return (int)n;
}

int main(int argc, char **argv) {
    printf("[evdi-bridge] Starting EVDI to Anland bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/display_daemon.sock", sizeof(addr.sun_path) - 1);

    unlink("/tmp/display_daemon.sock");
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[evdi-bridge] Failed to bind to /tmp/display_daemon.sock");
        return 1;
    }
    
    if (listen(sock, 1) < 0) {
        perror("[evdi-bridge] Failed to listen");
        return 1;
    }
    
    printf("[evdi-bridge] Listening on /tmp/display_daemon.sock. Waiting for Android app...\n");
    
    int client_sock = accept(sock, NULL, NULL);
    if (client_sock < 0) {
        perror("[evdi-bridge] Failed to accept");
        return 1;
    }

    printf("[evdi-bridge] Connected to Android app via display_daemon.sock!\n");

    // 1. Send PRODUCER_HELLO
    struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    write(client_sock, &hello, sizeof(hello));

    // 2. Send SCREEN_INFO (Tell Android what resolution EVDI is running at)
    struct {
        struct ctrl_msg hdr;
        struct screen_info info;
    } __attribute__((packed)) sinfo = {
        .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
        .info = { .width = 1080, .height = 2400, .format = 1, .refresh = 60000 }
    };
    write(client_sock, &sinfo, sizeof(sinfo));

    printf("[evdi-bridge] Handshake sent. Waiting for DMA-BUF FDs from Android...\n");

    // 3. Receive FDs
    struct ctrl_msg msg_buf;
    int fds[8];
    int fds_received = 0;
    while(1) {
        int n = recv_fds(client_sock, &msg_buf, sizeof(msg_buf), fds, 8, &fds_received);
        if (n <= 0) {
            perror("[evdi-bridge] Client disconnected or error");
            break;
        }
        printf("[evdi-bridge] Received message type: %d, FDs received: %d\n", msg_buf.type, fds_received);
        for(int i = 0; i < fds_received; i++) {
            printf("[evdi-bridge] Got DMA-BUF fd: %d\n", fds[i]);
        }
    }

    close(client_sock);
    close(sock);
    return 0;
}
