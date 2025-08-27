//
// Created by Tony Adriansen on 8/27/25.
//

#include "networking.h"

#include <assert.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

struct addrinfo* get_addr_info(const char* port, const char* host_name) {
  int status;
  struct addrinfo hints = {0};
  struct addrinfo* server_info = nullptr;
  hints.ai_family = AF_UNSPEC;  // don't care ipv4 or 6
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;  // fill my IP for me
  if ((status = getaddrinfo(host_name, port, &hints, &server_info)) != 0) {
    fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
    exit(1);
  }
  return server_info;
}

int open_and_listen_socket(struct addrinfo* addr_info) {
  int sock_fd = socket(addr_info->ai_family, addr_info->ai_socktype, addr_info->ai_protocol);
  if (sock_fd == -1) {
    fprintf(stderr, "socket error\n");
  }
  int yes = 1;
  setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);

  int status;
  if ((status = bind(sock_fd, addr_info->ai_addr, addr_info->ai_addrlen))) {
    perror("bind");
    return -1;
  }

  if ((status = listen(sock_fd, 20))) {
    perror("listen");
    return -1;
  }

  return sock_fd;
}
int frame_try_parse(Frame* frame, void* data, size_t size) {
  if (size < MSG_HDR_SIZE) {
    return 0;
  }

  uint32_t type;
  uint32_t len;
  memcpy(&type, data, sizeof(uint32_t));
  memcpy(&len, (uint8_t*)data + sizeof(uint32_t), 4);

  size_t frame_tot_size = len + MSG_HDR_SIZE;
  if (size < frame_tot_size) {
    return 0;
  }
  frame->payload = (uint8_t*)data + MSG_HDR_SIZE;
  frame->hdr.type = type;
  frame->hdr.len = len;
  return (int)(frame->hdr.len + MSG_HDR_SIZE);
}
ssize_t send_msg(int fd, int type, void* data, size_t size) {
  MsgHdr hdr;
  hdr.type = type;
  hdr.len = size;
  // hdr.type = htons(hdr.type);
  // hdr.len = htons(hdr.len);
  struct iovec iov[2];
  iov[0].iov_len = sizeof(hdr);
  iov[0].iov_base = &hdr;
  iov[1].iov_len = size;
  iov[1].iov_base = data;
  size_t tot_sent = 0;
  size_t tot_len = iov[0].iov_len + iov[1].iov_len;
  int iov_i = 0;
  size_t iov_sent = 0;
  while (tot_sent < tot_len) {
    ssize_t sent = writev(fd, &iov[iov_i], 2 - iov_i);
    if (sent == 0) {  // closed
      return sent;
    }
    if (sent < 0) {
      perror("writev");
      return sent;
    }
    tot_sent += sent;
    while (iov_i < 2 && iov_sent < tot_sent) {
      iov_sent += iov[iov_i].iov_len;
      iov_i++;
    }
  }
  return (ssize_t)tot_sent;
}

void msg_buf_init(MsgBuffer* buf, size_t cap) {
  assert(!buf->data);
  assert(!buf->cap);
  assert(cap > 0);
  *buf = (MsgBuffer){};
  msg_buf_reserve(buf, cap);
}

void msg_buf_reserve(MsgBuffer* buf, size_t len) {
  if (len > buf->cap) {
    void* new_data = malloc(len);
    if (!new_data) {
      perror("malloc");
      return;
    }
    if (buf->data) {
      memcpy(new_data, buf->data, buf->size);
      free(buf->data);
    }
    buf->data = new_data;
    buf->cap = len;
  }
}

void msg_buf_push(MsgBuffer* buf, int type, void* data, size_t len) {
  size_t tot_size = len + MSG_HDR_SIZE;
  size_t req_size = buf->size + tot_size;
  if (req_size > buf->cap) {
    msg_buf_reserve(buf, req_size > buf->cap * 2 ? req_size : buf->cap * 2);
  }
  MsgHdr hdr = {.type = type, .len = len};
  memcpy((uint8_t*)buf->data + buf->size, &hdr, sizeof(hdr));
  buf->size += sizeof(hdr);
  memcpy((uint8_t*)buf->data + buf->size, data, len);
  buf->size += len;
}

void msg_buf_clear(MsgBuffer* buf) { buf->size = 0; }

void msg_buf_free(MsgBuffer* buf) {
  free(buf->data);
  *buf = (MsgBuffer){};
}

ssize_t msg_buf_send_and_clear(MsgBuffer* buf, int fd) {
  if (buf->size <= 0) {
    return 0;
  }
  ssize_t sent = send(fd, buf->data, buf->size, 0);
  if (sent < 0) {
    perror("send");
  }
  msg_buf_clear(buf);
  return sent;
}
