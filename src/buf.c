//
// Created by Tony Adriansen on 8/26/25.
//

#include "buf.h"

void buf_init(Buf* buf, size_t cap) {
  assert(cap > 0);
  buf->data = malloc(cap);
  buf->size = 0;
  buf->cap = cap;
}

void buf_reserve(Buf* buf, size_t cap) {
  if (cap < 2048) {
    cap = 2048;
  }
  if (cap > buf->cap) {
    void* new_data = realloc(buf->data, cap);
    if (new_data == nullptr) {
      return;
    }
    buf->data = new_data;
    buf->cap = cap;
  }
}

ssize_t buf_recv(Buf* buf, int fd) {
  if (buf->cap - buf->size < 2048) {
    buf_reserve(buf, buf->size * 2);
  }
  ssize_t n = recv(fd, buf->data + buf->size, buf->cap - buf->size, 0);
  if (n > 0) {
    buf->size += n;
  }
  return n;
}

void buf_consume(Buf* buf, size_t size) {
  assert(buf->data);
  if (size >= buf->size) {
    buf->size = 0;
    return;
  }
  memmove(buf->data, buf->data + size, buf->size - size);
  buf->size -= size;
}

void buf_free(Buf* buf) {
  free(buf->data);
  buf->cap = 0;
  buf->size = 0;
}
