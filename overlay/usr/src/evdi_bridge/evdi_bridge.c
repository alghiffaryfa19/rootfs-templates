#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>

#define CTRL_MSG_CONSUMER_HELLO  1
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_FDS_READY      10
#define DATA_MSG_BUFS_READY      200

#define MAX_BUFS 8

struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct data_msg {
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

struct buf_info {
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
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

int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int main(int argc, char **argv) {
    printf("[evdi-bridge] Starting test renderer bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/var/display_daemon.sock", sizeof(addr.sun_path) - 1);
    unlink("/var/display_daemon.sock");
    
    // Also try /tmp for older configs
    unlink("/tmp/display_daemon.sock");
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        strncpy(addr.sun_path, "/tmp/display_daemon.sock", sizeof(addr.sun_path) - 1);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("[evdi-bridge] Failed to bind");
            return 1;
        }
    }
    chmod(addr.sun_path, 0777);
    listen(sock, 1);
    
    printf("[evdi-bridge] Listening on %s. Waiting for Android app...\n", addr.sun_path);
    int client_sock = accept(sock, NULL, NULL);
    if (client_sock < 0) return 1;
    printf("[evdi-bridge] Connected to Android app!\n");

    // 1. Send PRODUCER_HELLO
    struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    write(client_sock, &hello, sizeof(hello));

    // 2. Send SCREEN_INFO
    struct {
        struct ctrl_msg hdr;
        struct screen_info info;
    } __attribute__((packed)) sinfo = {
        .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
        .info = { .width = 1050, .height = 1726, .format = 1, .refresh = 60000 } // Adjust to native resolution
    };
    write(client_sock, &sinfo, sizeof(sinfo));

    // 3. Receive CONSUMER_HELLO and its 5 FDs
    struct ctrl_msg msg_buf;
    int conn_fds[8];
    int conn_fds_received = 0;
    if (recv_fds(client_sock, &msg_buf, sizeof(msg_buf), conn_fds, 8, &conn_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive CONSUMER_HELLO\n");
        return 1;
    }
    
    if (msg_buf.type != CTRL_MSG_CONSUMER_HELLO || conn_fds_received < 5) {
        printf("[evdi-bridge] Expected CONSUMER_HELLO (1) with 5 FDs, got type=%d fds=%d\n", msg_buf.type, conn_fds_received);
        // Sometimes it sends it in chunks, but let's assume it's here
    }

    int efd       = conn_fds[0];
    int fence_fd  = conn_fds[1];
    int data_fd   = conn_fds[2];
    int shm_fd    = conn_fds[3];
    int audio_fd  = conn_fds[4];

    printf("[evdi-bridge] Got connection FDs. data_fd=%d, shm_fd=%d, fence_fd=%d\n", data_fd, shm_fd, fence_fd);
    
    // Mmap the SHM
    volatile uint32_t *shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("[evdi-bridge] Failed to mmap shm");
        return 1;
    }

    // Tell Android we're ready
    struct ctrl_msg fds_ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
    write(client_sock, &fds_ready, sizeof(fds_ready));

    // 4. Wait for DATA_MSG_BUFS_READY on data_fd
    struct data_msg dmsg;
    int dma_fds[MAX_BUFS];
    int dma_fds_received = 0;
    
    printf("[evdi-bridge] Waiting for DMA-BUFs on data_fd...\n");
    if (recv_fds(data_fd, &dmsg, sizeof(dmsg), dma_fds, MAX_BUFS, &dma_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive DMA-BUFs\n");
        return 1;
    }
    if (dmsg.type != DATA_MSG_BUFS_READY) {
        printf("[evdi-bridge] Expected BUFS_READY (200), got %d\n", dmsg.type);
        return 1;
    }

    struct buf_info infos[MAX_BUFS];
    if (recv_all(data_fd, infos, dmsg.size) < 0) {
        printf("[evdi-bridge] Failed to read buf_infos\n");
        return 1;
    }

    printf("[evdi-bridge] Received %d DMA-BUFs!\n", dma_fds_received);

    // Mmap DMA-BUFs
    uint32_t *mapped_bufs[MAX_BUFS];
    for (int i = 0; i < dma_fds_received; i++) {
        size_t size = infos[i].stride * infos[i].height * 4; // assuming 4 bytes per pixel
        mapped_bufs[i] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fds[i], 0);
        if (mapped_bufs[i] == MAP_FAILED) {
            perror("[evdi-bridge] Failed to mmap DMA-BUF");
            return 1;
        }
        printf("[evdi-bridge] Mapped buffer %d (fd=%d, size=%zu, stride=%d, w=%d, h=%d)\n", 
            i, dma_fds[i], size, infos[i].stride, infos[i].width, infos[i].height);
    }

    // 5. Test Render Loop
    printf("[evdi-bridge] Starting test render loop...\n");
    uint8_t color_r = 0, color_g = 120, color_b = 255;
    
    while (1) {
        // We will just cycle through the buffers
        for (int i = 0; i < dma_fds_received; i++) {
            // Draw a solid color
            uint32_t *pixels = mapped_bufs[i];
            size_t num_pixels = infos[i].stride * infos[i].height;
            
            // Format is likely RGBA8888 or BGRA8888. Let's write a color.
            uint32_t color = (255 << 24) | (color_b << 16) | (color_g << 8) | color_r;
            for (size_t p = 0; p < num_pixels; p++) {
                pixels[p] = color;
            }
            
            // Animate color slightly
            color_r += 5;
            color_g += 2;
            color_b -= 3;
            
            // Tell Android this buffer is ready
            // According to Android code, it polls `buf_ready_efd` after checking `shm_ptr`?
            // Actually, Android waits for fence_fd.
            
            // Wait, does Android consumer expect us to write to fence_fd to say "frame is ready"?
            // Yes! `refresh_done` waits on `fence_fd`.
            
            // In Android side (display_consumer.c):
            // `select_dmabuf` writes to `buf_ready_efd` and sets `*shm_ptr = idx`.
            // Wait, if Android does `select_dmabuf`, Android TELLS US which buffer to draw to!
            
            // Let's poll on `buf_ready_efd` to wait for Android to give us a buffer!
            uint64_t efd_val;
            if (read(efd, &efd_val, sizeof(efd_val)) > 0) {
                uint32_t selected_idx = *shm_ptr;
                if (selected_idx < dma_fds_received) {
                    // Fill the selected buffer
                    uint32_t *p = mapped_bufs[selected_idx];
                    uint32_t c = (255 << 24) | ((color_b & 0xFF) << 16) | ((color_g & 0xFF) << 8) | (color_r & 0xFF);
                    for (size_t pxl = 0; pxl < infos[selected_idx].stride * infos[selected_idx].height; pxl++) {
                        p[pxl] = c;
                    }
                    color_r += 5; color_g += 2; color_b -= 3;
                    
                    // Signal frame is done by sending a byte over fence_fd
                    char dummy = 1;
                    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
                    struct msghdr fmsg = { .msg_iov = &iov, .msg_iovlen = 1 };
                    sendmsg(fence_fd, &fmsg, 0);
                }
            }
        }
    }

    return 0;
}
