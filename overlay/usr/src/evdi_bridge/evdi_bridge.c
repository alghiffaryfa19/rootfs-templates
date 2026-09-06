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

#define DRM_EVDI_CONNECT 0x00
#define DRM_EVDI_POLL 0x04
#define DRM_EVDI_GET_BUFF_CALLBACK 0x08
#define DRM_EVDI_DESTROY_BUFF_CALLBACK 0x09
#define DRM_EVDI_GBM_CREATE_BUFF_CALLBACK 0x0D

#define DRM_IOCTL_EVDI_CONNECT DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_CONNECT, struct drm_evdi_connect)
#define DRM_IOCTL_EVDI_POLL DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_POLL, struct drm_evdi_poll)
#define DRM_IOCTL_EVDI_GET_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GET_BUFF_CALLBACK, struct drm_evdi_get_buff_callabck)
#define DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_DESTROY_BUFF_CALLBACK, struct drm_evdi_destroy_buff_callback)
#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GBM_CREATE_BUFF_CALLBACK, struct drm_evdi_create_buff_callabck)

enum poll_event_type {
    none,
    add_buf,
    get_buf,
    destroy_buf,
    swap_to,
    create_buf
};

struct drm_evdi_connect {
    int32_t connected;
    int32_t dev_index;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
    uint32_t display_id;
};

struct drm_evdi_destroy_buff_callback {
    int poll_id;
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

int main() {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
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

    while (1) {
        int client_sock = accept(sock, NULL, NULL);
        if (client_sock < 0) {
            perror("[evdi-bridge] accept failed");
            continue;
        }
        printf("[evdi-bridge] Connected to Android app!\n");

        struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
        write(client_sock, &hello, sizeof(hello));

        // Wait for CONSUMER_HELLO and FDs
        struct ctrl_msg msg_buf;
        int conn_fds[8];
        int conn_fds_received = 0;
        if (recv_fds(client_sock, &msg_buf, sizeof(msg_buf), conn_fds, 8, &conn_fds_received) <= 0) {
            printf("[evdi-bridge] Failed to receive CONSUMER_HELLO\n");
            close(client_sock);
            continue;
        }


        int efd       = conn_fds[0];
        fence_fd      = conn_fds[1];
        int data_fd   = conn_fds[2];
        int shm_fd    = conn_fds[3];

        printf("[evdi-bridge] Got connection FDs. data_fd=%d, shm_fd=%d, fence_fd=%d\n", data_fd, shm_fd, fence_fd);
        
        shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) {
            perror("[evdi-bridge] Failed to mmap shm");
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            continue;
        }

        struct ctrl_msg fds_ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
        write(client_sock, &fds_ready, sizeof(fds_ready));

        struct data_msg dmsg;
        int dma_fds[MAX_BUFS];
        int dma_fds_received = 0;
        
        printf("[evdi-bridge] Waiting for DMA-BUFs on data_fd...\n");
        if (recv(data_fd, &msg_buf, sizeof(msg_buf), MSG_WAITALL) <= 0) {
            printf("[evdi-bridge] Failed to receive DMA-BUFs\n");
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            continue;
        }

        struct buf_info infos[MAX_BUFS];
        if (recv_all(data_fd, infos, dmsg.size) < 0) {
            printf("[evdi-bridge] Failed to read buf_infos\n");
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            continue;
        }

        printf("[evdi-bridge] Received %d DMA-BUFs! Real Size: %dx%d (Stride: %d bytes)\n", 
                dma_fds_received, infos[0].width, infos[0].height, infos[0].stride);

        uint32_t *mapped_bufs[MAX_BUFS];
        size_t map_sizes[MAX_BUFS];
        for (int i = 0; i < dma_fds_received; i++) {
            size_t calc_size = infos[i].stride * infos[i].height;
            off_t real_size = lseek(dma_fds[i], 0, SEEK_END);
            map_sizes[i] = (real_size > 0) ? (size_t)real_size : calc_size;
            
            mapped_bufs[i] = mmap(NULL, map_sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, dma_fds[i], 0);
            if (mapped_bufs[i] == MAP_FAILED) {
                perror("[evdi-bridge] Failed to mmap DMA-BUF");
                return 1;
            }
        }
        
        struct {
            struct ctrl_msg hdr;
            struct screen_info info;
        } __attribute__((packed)) sinfo = {
            .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
            .info = { .width = infos[0].width, .height = infos[0].height, .format = 1, .refresh = 60000 }
        };
        write(client_sock, &sinfo, sizeof(sinfo));

        evdi_handle evdi = EVDI_INVALID_HANDLE;
        int evdi_idx = -1;
        for (int i = 0; i < 10; i++) {
            evdi = evdi_open(i);
            if (evdi != EVDI_INVALID_HANDLE) {
                evdi_idx = i;
                break;
            }
        }

        if (evdi == EVDI_INVALID_HANDLE) {
            printf("[evdi-bridge] FATAL: Failed to open EVDI device.\n");
            return 1;
        }
        printf("[evdi-bridge] EVDI device %d opened successfully!\n", evdi_idx);

        int evdi_fd = evdi_get_event_ready(evdi);

        struct drm_evdi_connect cmd = {
            .connected = 1,
            .dev_index = 0,
            .width = infos[0].width,
            .height = infos[0].height,
            .refresh_rate = 60,
            .display_id = 0
        };
        if (drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_CONNECT, &cmd) < 0) {
            perror("[evdi-bridge] EVDI_CONNECT failed");
        }
        printf("[evdi-bridge] Connected display %dx%d\n", infos[0].width, infos[0].height);

        printf("[evdi-bridge] Bridge loop started. Waiting for EVDI_POLL events...\n");

        int buffer_assignment_index = 0;
        fd_set rfds;
        struct timeval tv;
        int max_fd = (evdi_fd > client_sock) ? evdi_fd : client_sock;
        bool connected = true;

        while (connected) {
            FD_ZERO(&rfds);
            FD_SET(evdi_fd, &rfds);
            FD_SET(client_sock, &rfds);
            
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0 && errno != EINTR) {
                perror("[evdi-bridge] select error");
                break;
            }
            
            if (ret > 0) {
                if (FD_ISSET(client_sock, &rfds)) {
                    char dummy;
                    if (recv(client_sock, &dummy, 1, MSG_PEEK) <= 0) {
                        printf("[evdi-bridge] Android app disconnected.\n");
                        connected = false;
                        break;
                    }
                }
                
                if (FD_ISSET(evdi_fd, &rfds)) {
                    struct drm_evdi_poll poll_cmd = {};
                    uint8_t poll_payload[32] = {0};
                    poll_cmd.data = poll_payload;

                    if (drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_POLL, &poll_cmd) < 0) {
                        if (errno != EINTR && errno != EAGAIN) {
                            perror("[evdi-bridge] EVDI_POLL error");
                            break;
                        }
                    } else {
                        if (poll_cmd.event == create_buf) {
                            struct drm_evdi_gbm_create_buff params;
                            memcpy(&params, poll_payload, sizeof(params));
                            int assigned_id = buffer_assignment_index % dma_fds_received;
                            buffer_assignment_index++;
                            struct drm_evdi_create_buff_callabck cb = {
                                .poll_id = poll_cmd.poll_id,
                                .id = assigned_id,
                                .stride = infos[assigned_id].stride
                            };
                            drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK, &cb);
                        } else if (poll_cmd.event == get_buf) {
                            int requested_id;
                            memcpy(&requested_id, poll_payload, sizeof(requested_id));
                            int fd_ints[1] = { dma_fds[requested_id] };
                            struct drm_evdi_get_buff_callabck cb = {
                                .poll_id = poll_cmd.poll_id,
                                .version = 1,
                                .numFds = 1,
                                .numInts = 0,
                                .fd_ints = fd_ints,
                                .data_ints = NULL
                            };
                            drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_GET_BUFF_CALLBACK, &cb);
                        } else if (poll_cmd.event == destroy_buf) {
                            struct drm_evdi_destroy_buff_callback cb = { .poll_id = poll_cmd.poll_id };
                            drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK, &cb);
                        } else if (poll_cmd.event == swap_to) {
                            char val = 1;
                            write(fence_fd, &val, 1);
                        }
                    }
                }
            }
        }
        struct drm_evdi_connect dis = {0};
        drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_CONNECT, &dis);
        evdi_close(evdi);

        // Cleanup resources before accepting new connection
        for (int i = 0; i < dma_fds_received; i++) {
            munmap(mapped_bufs[i], map_sizes[i]);
            close(dma_fds[i]);
        }
        munmap(shm_ptr, sizeof(uint32_t));
        close(client_sock);
        close(data_fd);
        close(shm_fd);
        close(fence_fd);
    }
    return 0;
}
