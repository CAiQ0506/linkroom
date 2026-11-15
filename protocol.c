#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

void frame_header(unsigned char header[FRAME_HEADER], uint16_t type, uint32_t length)
{
    uint16_t version = htons(1);
    uint16_t wire_type = htons(type);
    uint32_t wire_length = htonl(length);
    memcpy(header, &version, 2);
    memcpy(header + 2, &wire_type, 2);
    memcpy(header + 4, &wire_length, 4);
}

int send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *data = buffer;
    while (length) {
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        data += count;
        length -= (size_t)count;
    }
    return 0;
}

int recv_all(int fd, void *buffer, size_t length)
{
    unsigned char *data = buffer;
    size_t received = 0;
    while (received < length) {
        ssize_t count = recv(fd, data + received, length - received, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0) {
            if (!received)
                return 0;
            errno = EPROTO;
            return -1;
        }
        received += (size_t)count;
    }
    return 1;
}

int send_frame(int fd, uint16_t type, const char *text)
{
    unsigned char header[FRAME_HEADER];
    size_t length = strlen(text);
    if (length > CHAT_TEXT) {
        errno = EMSGSIZE;
        return -1;
    }
    frame_header(header, type, (uint32_t)length);
    if (send_all(fd, header, sizeof(header)) < 0)
        return -1;
    return send_all(fd, text, length);
}

int recv_frame(int fd, uint16_t *type, char *text, size_t capacity)
{
    unsigned char header[FRAME_HEADER];
    int status = recv_all(fd, header, sizeof(header));
    if (status <= 0)
        return status;

    uint16_t version, wire_type;
    uint32_t wire_length;
    memcpy(&version, header, 2);
    memcpy(&wire_type, header + 2, 2);
    memcpy(&wire_length, header + 4, 4);
    uint32_t length = ntohl(wire_length);
    if (ntohs(version) != 1 || length >= capacity) {
        errno = EPROTO;
        return -1;
    }
    if (recv_all(fd, text, length) != 1 || memchr(text, '\0', length)) {
        errno = EPROTO;
        return -1;
    }
    text[length] = '\0';
    *type = ntohs(wire_type);
    return 1;
}

int tcp_connect(const char *address, uint16_t port)
{
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, address, &peer.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        if (errno != EINPROGRESS)
            goto failure;
        struct pollfd wait = { .fd = fd, .events = POLLOUT };
        if (poll(&wait, 1, 5000) <= 0) {
            errno = ETIMEDOUT;
            goto failure;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
            goto failure;
        if (error) {
            errno = error;
            goto failure;
        }
    }
    if (fcntl(fd, F_SETFL, 0) < 0)
        goto failure;
    return fd;

failure:
    {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
}

int parse_number(const char *text, uint64_t maximum, uint64_t *value)
{
    uint64_t result = 0;
    if (!*text)
        return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9')
            return 0;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > maximum || result > (maximum - digit) / 10)
            return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}

int valid_name(const char *text)
{
    size_t length = strlen(text);
    if (!length || length > NAME_LIMIT)
        return 0;
    for (; *text; ++text) {
        if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') ||
              (*text >= '0' && *text <= '9') || *text == '_' || *text == '-'))
            return 0;
    }
    return 1;
}

int valid_text(const char *text)
{
    if (!*text)
        return 0;
    for (const unsigned char *byte = (const unsigned char *)text; *byte; ++byte) {
        if (*byte < 32 || *byte == 127)
            return 0;
    }
    return 1;
}

int valid_hex(const char *text, size_t length)
{
    return strlen(text) == length && strspn(text, "0123456789abcdef") == length;
}

int random_id(char out[ID_HEX + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[ID_HEX / 2];
    size_t used = 0;
    while (used < sizeof(bytes)) {
        ssize_t count = getrandom(bytes + used, sizeof(bytes) - used, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        used += (size_t)count;
    }
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[ID_HEX] = '\0';
    return 0;
}

void socket_timeout(int fd, int seconds)
{
    struct timeval timeout = { .tv_sec = seconds };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
