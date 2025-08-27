//
// Created by Tony Adriansen on 8/26/25.
//

#ifndef PROJECTWOW_BUF_H
#define PROJECTWOW_BUF_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct Buf {
  uint8_t* data;
  size_t size;
  size_t cap;
} Buf;

void buf_init(Buf* buf, size_t cap);

void buf_reserve(Buf* buf, size_t cap);

ssize_t buf_recv(Buf* buf, int fd);

void buf_consume(Buf* buf, size_t size);

void buf_free(Buf* buf);

#endif  // PROJECTWOW_BUF_H
