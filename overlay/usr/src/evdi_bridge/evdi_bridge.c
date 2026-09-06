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
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <evdi_lib.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DRM_EVDI_POLL 0x04
#define DRM_EVDI_GET_BUFF_CALLBACK 0x08
#define DRM_EVDI_GBM_CREATE_BUFF_CALLBACK 0x0A

#define DRM_IOCTL_EVDI_POLL DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_POLL, struct drm_evdi_poll)
#define DRM_IOCTL_EVDI_GET_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GET_BUFF_CALLBACK, struct drm_evdi_get_buff_callabck)
#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GBM_CREATE_BUFF_CALLBACK, struct drm_evdi_create_buff_callabck)

enum poll_event_type {
    none,
    add_buf,
    get_buf,
    destroy_buf,
    swap_to,
    create_buf
};

struct drm_evdi_poll {
    enum poll_event_type event;
    int poll_id;
    void *data;
};

struct drm_evdi_gbm_create_buff {
    int *id;
    uint32_t *stride;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct drm_evdi_create_buff_callabck {
    int poll_id;
    int id;
    uint32_t stride;
};

struct drm_evdi_get_buff_callabck {
    int poll_id;
    int version;
    int numFds;
    int numInts;
    int *fd_ints;
    int *data_ints;
};

int drm_ioctl(int fd, unsigned long req, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

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

// Global state
int fence_fd = -1;
volatile uint32_t *shm_ptr = NULL;

static void dpms_handler(int dpms_mode, void *user_data) {
    printf("[evdi-bridge] DPMS changed: %d\n", dpms_mode);
}

static void mode_changed_handler(struct evdi_mode mode, void *user_data) {
    printf("[evdi-bridge] Mode changed: %dx%d@%d\n", mode.width, mode.height, mode.refresh_rate);
}

static void update_ready_handler(int buffer_to_be_updated, void *user_data) {
    if (fence_fd >= 0) {
        char dummy = 1;
        struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
        struct msghdr fmsg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = NULL, .msg_controllen = 0 };
        sendmsg(fence_fd, &fmsg, 0);
    }
}

static void crtc_state_handler(int state, void *user_data) {
    printf("[evdi-bridge] CRTC state changed: %d\n", state);
}

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
    printf("[evdi-bridge] Starting EVDI bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/var/display_daemon.sock", sizeof(addr.sun_path) - 1);
    unlink("/var/display_daemon.sock");
    
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

    struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    write(client_sock, &hello, sizeof(hello));

    // Wait for CONSUMER_HELLO and FDs
    struct ctrl_msg msg_buf;
    int conn_fds[8];
    int conn_fds_received = 0;
    if (recv_fds(client_sock, &msg_buf, sizeof(msg_buf), conn_fds, 8, &conn_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive CONSUMER_HELLO\n");
        return 1;
    }
    
    int efd       = conn_fds[0];
    fence_fd      = conn_fds[1];
    int data_fd   = conn_fds[2];
    int shm_fd    = conn_fds[3];

    printf("[evdi-bridge] Got connection FDs. data_fd=%d, shm_fd=%d, fence_fd=%d\n", data_fd, shm_fd, fence_fd);
    
    shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("[evdi-bridge] Failed to mmap shm");
        return 1;
    }

    struct ctrl_msg fds_ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
    write(client_sock, &fds_ready, sizeof(fds_ready));

    // Wait for DMA-BUFs on data_fd
    struct data_msg dmsg;
    int dma_fds[MAX_BUFS];
    int dma_fds_received = 0;
    
    printf("[evdi-bridge] Waiting for DMA-BUFs on data_fd...\n");
    if (recv_fds(data_fd, &dmsg, sizeof(dmsg), dma_fds, MAX_BUFS, &dma_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive DMA-BUFs\n");
        return 1;
    }

    struct buf_info infos[MAX_BUFS];
    if (recv_all(data_fd, infos, dmsg.size) < 0) {
        printf("[evdi-bridge] Failed to read buf_infos\n");
        return 1;
    }

    printf("[evdi-bridge] Received %d DMA-BUFs! Real Size: %dx%d (Stride: %d bytes)\n", 
            dma_fds_received, infos[0].width, infos[0].height, infos[0].stride);

    // Mmap DMA-BUFs
    uint32_t *mapped_bufs[MAX_BUFS];
    for (int i = 0; i < dma_fds_received; i++) {
        size_t calc_size = infos[i].stride * infos[i].height;
        off_t real_size = lseek(dma_fds[i], 0, SEEK_END);
        size_t map_size = (real_size > 0) ? (size_t)real_size : calc_size;
        
        mapped_bufs[i] = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fds[i], 0);
        if (mapped_bufs[i] == MAP_FAILED) {
            perror("[evdi-bridge] Failed to mmap DMA-BUF");
            return 1;
        }
    }
    
    // Now send the dynamic SCREEN_INFO back to Android so it matches what we want!
    // But Android has already created the DMA-BUFs. We'll just echo it back just in case.
    struct {
        struct ctrl_msg hdr;
        struct screen_info info;
    } __attribute__((packed)) sinfo = {
        .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
        .info = { .width = infos[0].width, .height = infos[0].height, .format = 1, .refresh = 60000 }
    };
    write(client_sock, &sinfo, sizeof(sinfo));

    // Initialize EVDI
    int fd = open("/dev/dri/card1", O_RDWR);
    if (fd < 0) {
        printf("[evdi-bridge] FATAL: Failed to open EVDI device.\n");
        return 1;
    }
    printf("[evdi-bridge] EVDI device opened successfully!\n");

    // Connect EVDI
    struct drm_evdi_connect cmd = {
        .connected = 1,
        .dev_index = 0, // card1 usually has dev_index 0 in evdi context, or it ignores it
        .width = infos[0].width,
        .height = infos[0].height,
        .refresh_rate = 60,
        .display_id = 1
    };
    if (drm_ioctl(fd, DRM_IOCTL_EVDI_CONNECT, &cmd) < 0) {
        perror("[evdi-bridge] EVDI_CONNECT failed");
        return 1;
    }
    printf("[evdi-bridge] Connected display 3048x1906\n");

    printf("[evdi-bridge] Bridge loop started. Waiting for EVDI_POLL events...\n");

    int buffer_assignment_index = 0;

    struct pollfd pfds[2];
    pfds[0].fd = efd;       // Android efd
    pfds[0].events = POLLIN;
    pfds[1].fd = fd;        // DRM fd
    pfds[1].events = POLLIN;

    while (1) {
        struct drm_evdi_poll poll_cmd = {};
        uint8_t poll_payload[32] = {0};
        poll_cmd.data = poll_payload;

        int ret = drm_ioctl(fd, DRM_IOCTL_EVDI_POLL, &poll_cmd);
        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue; // retry
            }
            perror("[evdi-bridge] EVDI_POLL error");
            break;
        }

        if (poll_cmd.event == create_buf) {
            struct drm_evdi_gbm_create_buff params;
            memcpy(&params, poll_payload, sizeof(params));
            
            // Assign one of our Android DMA-BUFs (round-robin or just pick 0 for now)
            int assigned_id = buffer_assignment_index % dma_fds_received;
            buffer_assignment_index++;
            
            struct drm_evdi_create_buff_callabck cb = {
                .poll_id = poll_cmd.poll_id,
                .id = assigned_id,
                .stride = infos[assigned_id].stride
            };
            
            printf("[evdi-bridge] EVDI asks to create_buf. Assigning Android buffer ID %d\n", cb.id);
            if (drm_ioctl(fd, DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK, &cb) < 0) {
                perror("[evdi-bridge] GBM_CREATE_BUFF_CALLBACK failed");
            }
            
        } else if (poll_cmd.event == get_buf) {
            int requested_id;
            memcpy(&requested_id, poll_payload, sizeof(requested_id));
            
            // Give the kernel the actual DMA-BUF FD!
            int fd_ints[1] = { dma_fds[requested_id] };
            
            struct drm_evdi_get_buff_callabck cb = {
                .poll_id = poll_cmd.poll_id,
                .version = 1,
                .numFds = 1,
                .numInts = 0,
                .fd_ints = fd_ints,
                .data_ints = NULL
            };
            
            printf("[evdi-bridge] EVDI asks for get_buf ID %d. Sending FD %d\n", requested_id, fd_ints[0]);
            if (drm_ioctl(fd, DRM_IOCTL_EVDI_GET_BUFF_CALLBACK, &cb) < 0) {
                perror("[evdi-bridge] GET_BUFF_CALLBACK failed");
            }
            
        } else if (poll_cmd.event == destroy_buf) {
            // Nothing to do for destroy, just ack
            struct drm_evdi_destroy_buff_callback cb = { .poll_id = poll_cmd.poll_id };
            printf("[evdi-bridge] EVDI asks to destroy_buf\n");
            drm_ioctl(fd, DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK, &cb);
            
        } else if (poll_cmd.event == swap_to) {
            int swap_id;
            memcpy(&swap_id, poll_payload, sizeof(swap_id));
            // printf("[evdi-bridge] EVDI swap_to buffer %d\n", swap_id);
            
            // Tell Android the buffer is ready!
            uint64_t val = 1;
            write(fence_fd, &val, sizeof(val));
            
            // Consume Android's ready signal if available
            struct pollfd p = { .fd = efd, .events = POLLIN };
            if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
                uint64_t efd_val;
                read(efd, &efd_val, sizeof(efd_val));
            }
        }
    }

    struct drm_evdi_connect dis = {0};
    drm_ioctl(fd, DRM_IOCTL_EVDI_CONNECT, &dis);
    close(fd);
    return 0;
}
