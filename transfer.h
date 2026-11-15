#ifndef CHAT_TRANSFER_H
#define CHAT_TRANSFER_H

#include "protocol.h"

typedef const char *(*SessionLookup)(const char *token);
typedef int (*FileNotice)(const char *name, const char *id, const char *filename,
                          uint64_t size);

void serve_transfer(int fd, int directory, SessionLookup lookup, FileNotice notify);
int upload_file(const char *address, uint16_t port, const char *token, const char *path);
int download_file(const char *address, uint16_t port, const char *token,
                  const char *id, const char *path);

#endif
