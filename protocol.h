#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_HEADER 8
#define CHAT_INPUT 1024
#define CHAT_TEXT 2048
#define NAME_LIMIT 31
#define PASSWORD_LIMIT 256
#define ID_HEX 32
#define HASH_HEX 64
#define FILE_LIMIT (UINT64_C(1024) * 1024 * 1024)

enum Message {
    C_LOGIN = 1, C_GROUP, C_PRIVATE, C_WHO, C_HISTORY, C_QUIT,
    S_WELCOME = 20, S_EVENT, S_ERROR, S_END,
    F_UPLOAD = 40, F_DOWNLOAD, F_READY, F_RESULT,
    EV_FILE = 60
};

void frame_header(unsigned char header[FRAME_HEADER], uint16_t type, uint32_t length);
int send_all(int fd, const void *buffer, size_t length);
int recv_all(int fd, void *buffer, size_t length);
int send_frame(int fd, uint16_t type, const char *text);
int recv_frame(int fd, uint16_t *type, char *text, size_t capacity);
int tcp_connect(const char *address, uint16_t port);
int parse_number(const char *text, uint64_t maximum, uint64_t *value);
int valid_name(const char *text);
int valid_text(const char *text);
int valid_hex(const char *text, size_t length);
int random_id(char out[ID_HEX + 1]);
void socket_timeout(int fd, int seconds);

#endif
