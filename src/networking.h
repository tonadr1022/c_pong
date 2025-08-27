//
// Created by Tony Adriansen on 8/27/25.
//

#ifndef PONG_GAME_NETWORKING_H
#define PONG_GAME_NETWORKING_H

#include <stdint.h>
#include <stdio.h>

typedef struct MsgHdr {
  uint32_t type;
  uint32_t len;
} MsgHdr;

#define MSG_HDR_SIZE sizeof(MsgHdr)

typedef struct Frame {
  MsgHdr hdr;
  uint8_t* payload;
} Frame;

struct addrinfo* get_addr_info(const char* port, const char* host_name);

int open_and_listen_socket(struct addrinfo* addr_info);

int frame_try_parse(Frame* frame, void* data, size_t size);

ssize_t send_msg(int fd, int type, void* data, size_t size);

typedef struct MsgBuffer {
  void* data;
  size_t cap;
  size_t size;
} MsgBuffer;

void msg_buf_init(MsgBuffer* buf, size_t cap);
void msg_buf_reserve(MsgBuffer* buf, size_t len);
void msg_buf_push(MsgBuffer* buf, int type, void* data, size_t len);
void msg_buf_clear(MsgBuffer* buf);
void msg_buf_free(MsgBuffer* buf);
ssize_t msg_buf_send_and_clear(MsgBuffer* buf, int fd);

#define MSG_HDR_SIZE ((size_t)sizeof(MsgHdr))

#endif  // PONG_GAME_NETWORKING_H
