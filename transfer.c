#include "transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_CHUNK 65536
#define FILE_NAME_LIMIT 200

extern char **environ;

static int file_size(int fd, uint64_t *size)
{
    struct stat info;
    if (fstat(fd, &info) < 0)
        return -1;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > FILE_LIMIT) {
        errno = EINVAL;
        return -1;
    }
    *size = (uint64_t)info.st_size;
    return 0;
}

/* Hash the already-open file. No filenames or shell commands come from the wire. */
static int hash_file(int fd, char digest[HASH_HEX + 1])
{
    int channel[2];
    if (lseek(fd, 0, SEEK_SET) < 0 || pipe2(channel, O_CLOEXEC) < 0)
        return -1;

    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    pid_t child = -1;
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, fd, STDIN_FILENO);
        if (!result)
            result = posix_spawn_file_actions_adddup2(&actions, channel[1], STDOUT_FILENO);
        if (!result) {
            char *args[] = { "sha256sum", NULL };
            result = posix_spawn(&child, "/usr/bin/sha256sum", &actions, NULL, args, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    close(channel[1]);
    if (result) {
        close(channel[0]);
        errno = result;
        return -1;
    }

    char output[128];
    size_t used = 0;
    while (used < sizeof(output)) {
        ssize_t count = read(channel[0], output + used, sizeof(output) - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    close(channel[0]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) ||
        used < HASH_HEX + 1 || output[HASH_HEX] != ' ') {
        errno = EIO;
        return -1;
    }
    memcpy(digest, output, HASH_HEX);
    digest[HASH_HEX] = '\0';
    return valid_hex(digest, HASH_HEX) ? 0 : -1;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    while (length) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += count;
        length -= (size_t)count;
    }
    return 0;
}

static int stream_data(int socket, int file, uint64_t size, uint64_t offset,
                       int sending, int show_progress)
{
    unsigned char buffer[FILE_CHUNK];
    uint64_t position = offset;
    int last_percent = -1;
    if (lseek(file, (off_t)offset, SEEK_SET) < 0)
        return -1;
    if (show_progress)
        fprintf(stderr, "传输从 %" PRIu64 " / %" PRIu64 " 字节开始\n", offset, size);

    while (position < size) {
        size_t limit = size - position > sizeof(buffer) ? sizeof(buffer) : (size_t)(size - position);
        ssize_t count = sending ? read(file, buffer, limit) : recv(socket, buffer, limit, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (!count)
                errno = EIO;
            return -1;
        }
        if ((sending ? send_all(socket, buffer, (size_t)count)
                     : write_all(file, buffer, (size_t)count)) < 0)
            return -1;
        position += (uint64_t)count;
        int percent = (int)(position * 100 / size);
        if (show_progress && (percent >= last_percent + 10 || percent == 100)) {
            fprintf(stderr, "传输进度 %d%% (%" PRIu64 "/%" PRIu64 ")\n", percent, position, size);
            last_percent = percent;
        }
    }
    return 0;
}

static int split_fields(char *text, char **fields, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        fields[i] = text;
        char *separator = strchr(text, '\t');
        if (i == count - 1)
            return separator == NULL;
        if (!separator)
            return 0;
        *separator = '\0';
        text = separator + 1;
    }
    return 0;
}

static int valid_filename(const char *name)
{
    return strlen(name) <= FILE_NAME_LIMIT && valid_text(name) &&
           strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/') && !strchr(name, '\\');
}

static void clean_upload(int directory, const char *staging, int upload_dir)
{
    if (upload_dir >= 0) {
        unlinkat(upload_dir, "data", 0);
        unlinkat(upload_dir, "meta", 0);
        close(upload_dir);
    }
    unlinkat(directory, staging, AT_REMOVEDIR);
}

static void serve_upload(int socket, int directory, const char *user, char *fields[], FileNotice notify)
{
    uint64_t size;
    const char *digest = fields[2], *filename = fields[3];
    if (!parse_number(fields[1], FILE_LIMIT, &size) || !valid_hex(digest, HASH_HEX) ||
        !valid_filename(filename)) {
        send_frame(socket, S_ERROR, "上传参数无效");
        return;
    }

    char id[ID_HEX + 1], staging[ID_HEX + 10];
    if (random_id(id) < 0) {
        send_frame(socket, S_ERROR, "无法生成文件编号");
        return;
    }
    snprintf(staging, sizeof(staging), ".upload-%s", id);
    if (mkdirat(directory, staging, 0700) < 0) {
        send_frame(socket, S_ERROR, "无法准备上传目录");
        return;
    }

    int upload_dir = openat(directory, staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int data = upload_dir < 0 ? -1 : openat(upload_dir, "data",
                                           O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int meta = -1;
    const char *error = "上传或写入失败";
    char actual[HASH_HEX + 1];
    if (data < 0 || send_frame(socket, F_READY, id) < 0 ||
        stream_data(socket, data, size, 0, 0, 0) < 0 || fsync(data) < 0)
        goto failure;
    if (hash_file(data, actual) < 0 || strcmp(actual, digest)) {
        error = "上传文件 SHA-256 校验失败";
        goto failure;
    }

    meta = openat(upload_dir, "meta", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    char record[512];
    int length = snprintf(record, sizeof(record), "%" PRIu64 "\t%s\t%s", size, digest, filename);
    if (meta < 0 || write_all(meta, record, (size_t)length) < 0 || fsync(meta) < 0)
        goto failure;
    close(meta);
    close(data);
    close(upload_dir);
    meta = data = upload_dir = -1;
    if (renameat(directory, staging, directory, id) < 0) {
        upload_dir = openat(directory, staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        goto failure;
    }

    /* Report the stored file to the parent using the chat event queue. */
    if (notify(user, id, filename, size) < 0) {
        snprintf(record, sizeof(record), "文件已保存，群通知发送失败，文件 ID %s", id);
        send_frame(socket, S_ERROR, record);
        return;
    }
    send_frame(socket, F_RESULT, id);
    return;

failure:
    if (!strcmp(error, "上传或写入失败"))
        perror("保存群文件");
    if (meta >= 0)
        close(meta);
    if (data >= 0)
        close(data);
    clean_upload(directory, staging, upload_dir);
    send_frame(socket, S_ERROR, error);
}

static void serve_download(int socket, int directory, char *fields[])
{
    const char *id = fields[1];
    uint64_t offset;
    if (!valid_hex(id, ID_HEX) || !parse_number(fields[2], FILE_LIMIT, &offset)) {
        send_frame(socket, S_ERROR, "文件编号或偏移无效");
        return;
    }

    int folder = openat(directory, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0) {
        send_frame(socket, S_ERROR, "文件不存在");
        return;
    }
    int data = openat(folder, "data", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int meta = openat(folder, "meta", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    char record[512];
    ssize_t count = meta < 0 ? -1 : read(meta, record, sizeof(record) - 1);
    if (count >= 0)
        record[count] = '\0';
    char *info[3];
    uint64_t size, stored_size;
    if (count <= 0 || !split_fields(record, info, 3) ||
        !parse_number(info[0], FILE_LIMIT, &size) || !valid_hex(info[1], HASH_HEX) ||
        !valid_filename(info[2]) || data < 0 || file_size(data, &stored_size) < 0 ||
        size != stored_size || offset > size) {
        send_frame(socket, S_ERROR, "文件不可用或本地偏移超过总长度");
    } else {
        char header[512];
        snprintf(header, sizeof(header), "%" PRIu64 "\t%s\t%s", size, info[1], info[2]);
        if (send_frame(socket, F_READY, header) == 0 &&
            stream_data(socket, data, size, offset, 1, 0) == 0)
            send_frame(socket, F_RESULT, id);
    }
    if (data >= 0)
        close(data);
    if (meta >= 0)
        close(meta);
    close(folder);
}

void serve_transfer(int fd, int directory, SessionLookup lookup, FileNotice notify)
{
    char request[CHAT_INPUT + 1], *fields[4];
    uint16_t type;
    socket_timeout(fd, 20);
    if (recv_frame(fd, &type, request, sizeof(request)) != 1)
        return;
    if ((type != F_UPLOAD && type != F_DOWNLOAD) ||
        !split_fields(request, fields, type == F_UPLOAD ? 4 : 3)) {
        send_frame(fd, S_ERROR, "文件请求无效");
        return;
    }
    const char *owner = valid_hex(fields[0], ID_HEX) ? lookup(fields[0]) : NULL;
    if (!owner) {
        send_frame(fd, S_ERROR, "聊天会话无效，请重新登录");
        return;
    }
    if (type == F_UPLOAD)
        serve_upload(fd, directory, owner, fields, notify);
    else
        serve_download(fd, directory, fields);
}

static int expect(int fd, uint16_t expected, char response[CHAT_TEXT + 1])
{
    uint16_t actual;
    if (recv_frame(fd, &actual, response, CHAT_TEXT + 1) != 1) {
        fprintf(stderr, "文件连接中断或响应超时\n");
        return -1;
    }
    if (actual == S_ERROR) {
        fprintf(stderr, "文件服务：%s\n", response);
        return -1;
    }
    if (actual != expected) {
        fprintf(stderr, "文件服务返回了意外的消息类型\n");
        return -1;
    }
    return 0;
}

int upload_file(const char *address, uint16_t port, const char *token, const char *path)
{
    const char *base = strrchr(path, '/');
    const char *name = base ? base + 1 : path;
    if (!valid_filename(name)) {
        fprintf(stderr, "文件名无效或超过 %d 字节\n", FILE_NAME_LIMIT);
        return -1;
    }

    int file = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) {
        perror("打开上传文件");
        return -1;
    }
    int status = -1, socket = -1;
    uint64_t size;
    char digest[HASH_HEX + 1], request[CHAT_INPUT + 1], response[CHAT_TEXT + 1];
    if (file_size(file, &size) < 0 || hash_file(file, digest) < 0) {
        fprintf(stderr, "无法读取或计算文件校验值，文件上限为 1 GiB\n");
        goto end;
    }
    socket = tcp_connect(address, port);
    if (socket < 0) {
        perror("连接文件服务");
        goto end;
    }
    socket_timeout(socket, 30);
    snprintf(request, sizeof(request), "%s\t%" PRIu64 "\t%s\t%s", token, size, digest, name);
    if (send_frame(socket, F_UPLOAD, request) < 0 || expect(socket, F_READY, response) < 0)
        goto end;
    if (stream_data(socket, file, size, 0, 1, 1) < 0) {
        perror("上传中断");
        goto end;
    }
    if (expect(socket, F_RESULT, response) < 0)
        goto end;
    fprintf(stderr, "上传完成，文件 ID %s\n", response);
    status = 0;

end:
    if (socket >= 0)
        close(socket);
    close(file);
    return status;
}

int download_file(const char *address, uint16_t port, const char *token,
                  const char *id, const char *path)
{
    struct stat existing;
    if (lstat(path, &existing) == 0 || errno != ENOENT) {
        fprintf(stderr, "目标路径已存在或不可用：%s\n", path);
        return -1;
    }
    char *partial = NULL;
    if (asprintf(&partial, "%s.part-%s", path, id) < 0)
        return -1;
    int file = open(partial, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    int status = -1, socket = -1;
    uint64_t offset, total;
    char request[CHAT_INPUT + 1], response[CHAT_TEXT + 1], digest[HASH_HEX + 1];
    char *info[3];
    if (file < 0 || flock(file, LOCK_EX | LOCK_NB) < 0 || file_size(file, &offset) < 0) {
        perror("打开下载临时文件");
        goto end;
    }
    socket = tcp_connect(address, port);
    if (socket < 0) {
        perror("连接文件服务");
        goto end;
    }
    socket_timeout(socket, 30);
    snprintf(request, sizeof(request), "%s\t%s\t%" PRIu64, token, id, offset);
    if (send_frame(socket, F_DOWNLOAD, request) < 0 || expect(socket, F_READY, response) < 0)
        goto end;
    if (!split_fields(response, info, 3) || !parse_number(info[0], FILE_LIMIT, &total) ||
        !valid_hex(info[1], HASH_HEX) || total < offset) {
        fprintf(stderr, "下载元数据无效\n");
        goto end;
    }
    memcpy(digest, info[1], sizeof(digest));
    if (stream_data(socket, file, total, offset, 0, 1) < 0) {
        fprintf(stderr, "下载中断，保留临时文件 %s，使用原命令继续\n", partial);
        goto end;
    }
    if (expect(socket, F_RESULT, response) < 0)
        goto end;

    char actual[HASH_HEX + 1];
    if (hash_file(file, actual) < 0 || strcmp(digest, actual)) {
        fprintf(stderr, "下载校验失败，请移走临时文件 %s 后重新下载\n", partial);
        goto end;
    }
    if (fsync(file) < 0 || link(partial, path) < 0) {
        perror("保存下载文件");
        goto end;
    }
    if (unlink(partial) < 0)
        perror("清理下载临时文件");
    fprintf(stderr, "下载完成，SHA-256 校验通过：%s\n", path);
    status = 0;

end:
    if (socket >= 0)
        close(socket);
    if (file >= 0)
        close(file);
    free(partial);
    return status;
}
