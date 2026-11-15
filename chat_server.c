#include "protocol.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/msg.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_TRANSFERS 8
#define HISTORY_LIMIT 50
#define HISTORY_REPLY 20
#define SEND_CAPACITY (64 * 1024)
#define QUEUE_BATCH 128

struct Session {
    int fd;
    int closing;
    pid_t child;
    uint64_t generation;
    char name[NAME_LIMIT + 1];
    char token[ID_HEX + 1];
    size_t sent, end;
    unsigned char output[SEND_CAPACITY];
};

struct QueueMessage {
    long mtype;
    uint64_t generation;
    uint16_t kind;
    char text[CHAT_INPUT + 1];
};

static struct Session sessions[MAX_CLIENTS];
static pid_t transfers[MAX_TRANSFERS];
static uint64_t next_generation;
static char history[HISTORY_LIMIT][CHAT_TEXT + 1];
static size_t history_count, history_next;
static int queue = -1, wakeup[2] = { -1, -1 };
static int listener = -1, file_listener = -1, signals = -1;
static int lock_fd = -1, files_fd = -1;
static FILE *history_file;
static const char *storage_dir;
static char account_helper[PATH_MAX];

static void disconnect(struct Session *session)
{
    if (session->fd < 0)
        return;
    if (session->name[0])
        fprintf(stderr, "下线 %s (PID %ld)\n", session->name, (long)session->child);
    shutdown(session->fd, SHUT_RDWR);
    close(session->fd);
    session->fd = -1;
    session->name[0] = '\0';
    session->token[0] = '\0';
}

static int push_frame(struct Session *session, uint16_t kind, const char *text)
{
    if (session->fd < 0)
        return -1;
    size_t length = strlen(text);
    size_t remaining = session->end - session->sent;
    if (remaining + FRAME_HEADER + length > sizeof(session->output)) {
        fprintf(stderr, "发送缓冲区已满，关闭慢连接\n");
        disconnect(session);
        return -1;
    }
    if (session->end + FRAME_HEADER + length > sizeof(session->output)) {
        memmove(session->output, session->output + session->sent, remaining);
        session->end = remaining;
        session->sent = 0;
    }

    frame_header(session->output + session->end, kind, (uint32_t)length);
    memcpy(session->output + session->end + FRAME_HEADER, text, length);
    session->end += FRAME_HEADER + length;
    return 0;
}

static void flush_session(struct Session *session)
{
    size_t remaining = session->end - session->sent;
    if (remaining) {
        ssize_t sent = send(session->fd, session->output + session->sent, remaining,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (sent <= 0) {
            disconnect(session);
            return;
        }
        session->sent += (size_t)sent;
    }
    if (session->sent == session->end) {
        session->sent = session->end = 0;
        if (session->closing)
            disconnect(session);
    }
}

static struct Session *find_user(const char *name)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd >= 0 && !sessions[i].closing &&
            sessions[i].name[0] && !strcmp(sessions[i].name, name))
            return &sessions[i];
    }
    return NULL;
}

static const char *lookup_token(const char *token)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd >= 0 && !sessions[i].closing &&
            sessions[i].token[0] && !strcmp(sessions[i].token, token))
            return sessions[i].name;
    }
    return NULL;
}

static void stamp(char result[32])
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(result, 32, "%Y-%m-%d %H:%M:%S", &local);
}

static void remember(const char *line)
{
    snprintf(history[history_next], sizeof(history[history_next]), "%s", line);
    history_next = (history_next + 1) % HISTORY_LIMIT;
    if (history_count < HISTORY_LIMIT)
        ++history_count;
}

static void broadcast(const char *line)
{
    if (fprintf(history_file, "%s\n", line) < 0 || fflush(history_file) < 0) {
        perror("写入聊天历史");
        clearerr(history_file);
    }
    remember(line);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd >= 0 && sessions[i].name[0] && !sessions[i].closing)
            push_frame(&sessions[i], S_EVENT, line);
    }
}

static void reply_history(struct Session *session, const char *request)
{
    uint64_t amount = 10;
    if (*request && (!parse_number(request, HISTORY_REPLY, &amount) || !amount)) {
        push_frame(session, S_ERROR, "历史条数需为 1–20");
        return;
    }
    if (amount > history_count)
        amount = history_count;
    size_t start = (history_next + HISTORY_LIMIT - (size_t)amount) % HISTORY_LIMIT;
    for (size_t i = 0; i < amount; ++i) {
        if (push_frame(session, S_EVENT, history[(start + i) % HISTORY_LIMIT]) < 0)
            return;
    }
    push_frame(session, S_END, "历史查询结束");
}

static void reply_online(struct Session *session)
{
    char text[CHAT_TEXT + 1] = "在线用户";
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd >= 0 && !sessions[i].closing && sessions[i].name[0]) {
            strcat(text, " ");
            strcat(text, sessions[i].name);
        }
    }
    push_frame(session, S_EVENT, text);
}

static void handle_login(struct Session *session, const char *text)
{
    if (!valid_name(text) || find_user(text) || random_id(session->token) < 0) {
        push_frame(session, S_ERROR, "登录失败：用户名无效、已在线或无法建立会话");
        session->closing = 1;
        return;
    }
    memcpy(session->name, text, strlen(text) + 1);
    push_frame(session, S_WELCOME, session->token);
    fprintf(stderr, "上线 %s (PID %ld)\n", session->name, (long)session->child);
}

static void handle_private(struct Session *session, char *request, const char *time)
{
    char *separator = strchr(request, ' ');
    if (!separator) {
        push_frame(session, S_ERROR, "用法 /msg 用户名 消息");
        return;
    }
    *separator++ = '\0';
    struct Session *target = valid_name(request) ? find_user(request) : NULL;
    if (!target || !valid_text(separator)) {
        push_frame(session, S_ERROR, "私聊对象不在线或消息无效");
        return;
    }
    char line[CHAT_TEXT + 1];
    snprintf(line, sizeof(line), "[%s] [私聊] %s → %s: %s",
             time, session->name, target->name, separator);
    if (push_frame(target, S_EVENT, line) < 0) {
        push_frame(session, S_ERROR, "私聊投递失败");
        return;
    }
    if (target != session)
        push_frame(session, S_EVENT, line);
}

static void handle_message(struct QueueMessage *message)
{
    if (message->kind == EV_FILE) {
        broadcast(message->text);
        return;
    }
    struct Session *session = NULL;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].generation == message->generation) {
            session = &sessions[i];
            break;
        }
    }
    if (!session || session->fd < 0 || session->closing)
        return;
    if (message->kind == C_QUIT) {
        push_frame(session, S_END, "已退出");
        session->closing = 1;
        return;
    }
    if (!session->name[0]) {
        if (message->kind == C_LOGIN)
            handle_login(session, message->text);
        else {
            push_frame(session, S_ERROR, "请先登录");
            session->closing = 1;
        }
        return;
    }

    char time[32], line[CHAT_TEXT + 1];
    stamp(time);
    switch (message->kind) {
    case C_GROUP:
        if (valid_text(message->text)) {
            snprintf(line, sizeof(line), "[%s] [群聊] %s: %s", time, session->name, message->text);
            broadcast(line);
        } else {
            push_frame(session, S_ERROR, "群聊内容为空或含控制字符");
        }
        break;
    case C_PRIVATE:
        handle_private(session, message->text, time);
        break;
    case C_WHO:
        reply_online(session);
        break;
    case C_HISTORY:
        reply_history(session, message->text);
        break;
    default:
        push_frame(session, S_ERROR, "未知命令");
        break;
    }
}

/* A byte wakes poll(); the queue holds the complete request. A full pipe is
   already readable, so a missed wake byte never loses a queued message. */
static int enqueue(uint64_t generation, uint16_t kind, const char *text)
{
    struct QueueMessage message = { .mtype = 1, .generation = generation, .kind = kind };
    snprintf(message.text, sizeof(message.text), "%s", text);
    size_t length = offsetof(struct QueueMessage, text) - sizeof(message.mtype) +
                    strlen(message.text) + 1;
    if (msgsnd(queue, &message, length, IPC_NOWAIT) < 0)
        return -1;
    ssize_t written;
    do {
        written = write(wakeup[1], ".", 1);
    } while (written < 0 && errno == EINTR);
    return 0;
}

static int notify_file(const char *user, const char *id, const char *name, uint64_t size)
{
    char line[CHAT_INPUT + 1], time[32];
    stamp(time);
    snprintf(line, sizeof(line), "[%s] [群文件] %s 上传 %s (%" PRIu64 " 字节)，ID %s",
             time, user, name, size, id);
    for (int attempts = 0; attempts < 20; ++attempts) {
        if (enqueue(0, EV_FILE, line) == 0)
            return 0;
        if (errno != EAGAIN)
            break;
        usleep(10000);
    }
    return -1;
}

static int drain_queue(void)
{
    char bytes[1024];
    /* Consume only a bounded amount before serving other sockets. */
    for (int i = 0; i < 8 && read(wakeup[0], bytes, sizeof(bytes)) > 0; ++i) {
    }
    for (int i = 0; i < QUEUE_BATCH; ++i) {
        struct QueueMessage message = { 0 };
        if (msgrcv(queue, &message, sizeof(message) - sizeof(message.mtype), 1, IPC_NOWAIT) < 0)
            return 0;
        handle_message(&message);
    }
    return 1;
}

static void child_setup(int keep)
{
    close(listener);
    close(file_listener);
    close(signals);
    close(wakeup[0]);
    close(lock_fd);
    close(fileno(history_file));
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd >= 0 && sessions[i].fd != keep)
            close(sessions[i].fd);
    }
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

static int locate_account_helper(void)
{
    ssize_t length = readlink("/proc/self/exe", account_helper, sizeof(account_helper) - 1);
    if (length < 0)
        return -1;
    account_helper[length] = '\0';
    char *slash = strrchr(account_helper, '/');
    static const char suffix[] = "/web/accounts.py";
    if (!slash || (size_t)(slash - account_helper) + sizeof(suffix) > sizeof(account_helper)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(slash, suffix, sizeof(suffix));
    return access(account_helper, R_OK);
}

static int verify_password(const char *name, const char *password)
{
    int channel[2];
    if (pipe2(channel, O_CLOEXEC) < 0)
        return -1;
    size_t length = strlen(password);
    ssize_t written;
    do {
        written = write(channel[1], password, length);
    } while (written < 0 && errno == EINTR);
    close(channel[1]);
    if (written != (ssize_t)length) {
        close(channel[0]);
        return -1;
    }

    pid_t child = fork();
    if (child == 0) {
        if (channel[0] != STDIN_FILENO) {
            if (dup2(channel[0], STDIN_FILENO) < 0)
                _exit(2);
            close(channel[0]);
        } else if (fcntl(STDIN_FILENO, F_SETFD, 0) < 0) {
            _exit(2);
        }
        execl("/usr/bin/python3", "python3", "-I", "-B", account_helper, storage_dir, name, (char *)NULL);
        _exit(2);
    }
    close(channel[0]);
    if (child < 0)
        return -1;
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) > 1)
        return -1;
    return WEXITSTATUS(status) == 0;
}

static void run_chat_child(int socket, uint64_t generation)
{
    char text[CHAT_INPUT + 1];
    uint16_t type;
    socket_timeout(socket, 10);
    if (recv_frame(socket, &type, text, sizeof(text)) != 1)
        return;
    char *password = strchr(text, '\t');
    int verified = 0;
    if (type == C_LOGIN && password) {
        *password++ = '\0';
        if (valid_name(text) && valid_text(password) && strlen(password) <= PASSWORD_LIMIT)
            verified = verify_password(text, password);
        explicit_bzero(password, strlen(password));
    }
    if (verified <= 0) {
        send_frame(socket, S_ERROR, verified < 0
                   ? "账号服务暂不可用，请先启动网页网关"
                   : "用户名或密码不正确。首次使用请先在网页注册。");
        return;
    }
    /* Only send the verified username to the parent process. */
    if (enqueue(generation, C_LOGIN, text) < 0)
        return;
    socket_timeout(socket, 0);
    while (recv_frame(socket, &type, text, sizeof(text)) == 1) {
        /* Only chat operations may come from a client. File notices are internal. */
        if (type < C_GROUP || type > C_QUIT)
            type = 0;
        if (enqueue(generation, type, text) < 0 || type == C_QUIT)
            return;
    }
}

static void reap_children(void)
{
    pid_t child;
    while ((child = waitpid(-1, NULL, WNOHANG)) > 0) {
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            if (sessions[i].child == child)
                sessions[i].child = 0;
        }
        for (size_t i = 0; i < MAX_TRANSFERS; ++i) {
            if (transfers[i] == child)
                transfers[i] = 0;
        }
    }
}

static void close_finished_sessions(void)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        struct Session *session = &sessions[i];
        if (session->fd >= 0 && (!session->child || session->closing)) {
            if (!session->closing) {
                disconnect(session);
                continue;
            }
            /* Try once to flush an exit reply, then release the connection. */
            flush_session(session);
            if (session->fd >= 0)
                disconnect(session);
        }
    }
}

static int open_listener(const char *address, uint16_t port)
{
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, address, &local.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(fd, 32) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static void accept_chat(void)
{
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0)
        return;

    struct Session *slot = NULL;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].fd < 0 && !sessions[i].child) {
            slot = &sessions[i];
            break;
        }
    }
    if (!slot) {
        socket_timeout(client, 1);
        send_frame(client, S_ERROR, "聊天室连接数已满");
        close(client);
        return;
    }

    uint64_t generation = ++next_generation;
    pid_t child = fork();
    if (child == 0) {
        child_setup(client);
        close(files_fd);
        run_chat_child(client, generation);
        close(client);
        _exit(0);
    }
    if (child < 0) {
        perror("创建连接子进程");
        close(client);
        return;
    }
    *slot = (struct Session){ .fd = client, .child = child, .generation = generation };
}

static void accept_transfer(void)
{
    int client = accept4(file_listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0)
        return;
    size_t slot = 0;
    while (slot < MAX_TRANSFERS && transfers[slot])
        ++slot;
    if (slot == MAX_TRANSFERS) {
        socket_timeout(client, 1);
        send_frame(client, S_ERROR, "文件服务繁忙，请稍后重试");
        close(client);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        child_setup(client);
        serve_transfer(client, files_fd, lookup_token, notify_file);
        close(client);
        _exit(0);
    }
    if (child < 0)
        perror("创建文件传输子进程");
    else
        transfers[slot] = child;
    close(client);
}

static void clean_staging(void)
{
    int copy = openat(files_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0)
        return;
    DIR *directory = fdopendir(copy);
    if (!directory) {
        close(copy);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, ".upload-", 8) || !valid_hex(entry->d_name + 8, ID_HEX))
            continue;
        int staging = openat(files_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (staging < 0)
            continue;
        unlinkat(staging, "data", 0);
        unlinkat(staging, "meta", 0);
        close(staging);
        unlinkat(files_fd, entry->d_name, AT_REMOVEDIR);
    }
    closedir(directory);
}

static int open_storage(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    int root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0)
        return -1;
    lock_fd = openat(root, ".lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
        goto failure;
    if (mkdirat(root, "files", 0700) < 0 && errno != EEXIST)
        goto failure;
    files_fd = openat(root, "files", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (files_fd < 0)
        goto failure;

    int log = openat(root, "history.log", O_CREAT | O_RDWR | O_APPEND | O_NOFOLLOW |
                                         O_NONBLOCK | O_CLOEXEC, 0600);
    if (log < 0)
        goto failure;
    struct stat info;
    if (fstat(log, &info) < 0 || !S_ISREG(info.st_mode)) {
        close(log);
        errno = EINVAL;
        goto failure;
    }
    history_file = fdopen(log, "a+");
    if (!history_file) {
        close(log);
        goto failure;
    }
    close(root);
    clean_staging();

    /* Only the tail of the journal is needed for /history. */
    off_t tail = info.st_size > HISTORY_LIMIT * (CHAT_TEXT + 1)
                     ? info.st_size - HISTORY_LIMIT * (CHAT_TEXT + 1) : 0;
    if (fseeko(history_file, tail, SEEK_SET) < 0)
        return -1;
    char line[CHAT_TEXT + 2];
    if (tail && !fgets(line, sizeof(line), history_file))
        return -1;
    while (fgets(line, sizeof(line), history_file)) {
        size_t length = strlen(line);
        if (length && line[length - 1] == '\n') {
            line[length - 1] = '\0';
            remember(line);
        }
    }
    if (ferror(history_file))
        return -1;
    return fseeko(history_file, 0, SEEK_END);

failure:
    {
        int error = errno;
        close(root);
        errno = error;
        return -1;
    }
}

static int run_server(void)
{
    int pending = 0;
    for (;;) {
        struct pollfd watches[4 + MAX_CLIENTS] = {
            { .fd = listener, .events = POLLIN },
            { .fd = file_listener, .events = POLLIN },
            { .fd = signals, .events = POLLIN },
            { .fd = wakeup[0], .events = POLLIN }
        };
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            watches[4 + i].fd = sessions[i].sent < sessions[i].end ? sessions[i].fd : -1;
            watches[4 + i].events = POLLOUT;
        }
        int ready = poll(watches, 4 + MAX_CLIENTS, pending ? 0 : -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (watches[2].revents & POLLIN) {
            struct signalfd_siginfo event;
            while (read(signals, &event, sizeof(event)) == sizeof(event)) {
                if (event.ssi_signo == SIGINT || event.ssi_signo == SIGTERM)
                    return 0;
                reap_children();
            }
        }
        pending = drain_queue();
        if (!pending)
            close_finished_sessions();
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            if (sessions[i].fd >= 0 && watches[4 + i].revents)
                flush_session(&sessions[i]);
        }
        if (watches[0].revents & POLLIN)
            accept_chat();
        if (watches[1].revents & POLLIN)
            accept_transfer();
    }
}

static void shutdown_server(void)
{
    if (listener >= 0)
        close(listener);
    if (file_listener >= 0)
        close(file_listener);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        disconnect(&sessions[i]);
        if (sessions[i].child)
            kill(sessions[i].child, SIGTERM);
    }
    for (size_t i = 0; i < MAX_TRANSFERS; ++i) {
        if (transfers[i])
            kill(transfers[i], SIGTERM);
    }
    if (queue >= 0)
        msgctl(queue, IPC_RMID, NULL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
    }
    if (signals >= 0)
        close(signals);
    if (wakeup[0] >= 0)
        close(wakeup[0]);
    if (wakeup[1] >= 0)
        close(wakeup[1]);
    if (files_fd >= 0) {
        clean_staging();
        close(files_fd);
    }
    if (history_file)
        fclose(history_file);
    if (lock_fd >= 0)
        close(lock_fd);
}

int main(int argc, char **argv)
{
    const char *address = argc > 1 ? argv[1] : "127.0.0.1";
    storage_dir = argc > 3 ? argv[3] : "chat-data";
    uint64_t port = 9000;
    if (argc > 4 || (argc > 2 && (!parse_number(argv[2], 65534, &port) || !port))) {
        fprintf(stderr, "用法 %s [IPv4地址] [聊天端口] [数据目录]\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i)
        sessions[i].fd = -1;

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGCHLD);
    int status = 1;
    if (sigprocmask(SIG_BLOCK, &blocked, NULL) < 0 ||
        (signals = signalfd(-1, &blocked, SFD_NONBLOCK | SFD_CLOEXEC)) < 0 ||
        locate_account_helper() < 0 ||
        open_storage(storage_dir) < 0 ||
        pipe2(wakeup, O_NONBLOCK | O_CLOEXEC) < 0 ||
        (queue = msgget(IPC_PRIVATE, IPC_CREAT | 0600)) < 0 ||
        (listener = open_listener(address, (uint16_t)port)) < 0 ||
        (file_listener = open_listener(address, (uint16_t)(port + 1))) < 0) {
        perror("启动服务器");
    } else {
        printf("聊天 %s:%" PRIu64 "，文件端口 %" PRIu64 "，消息队列 %d\n",
               address, port, port + 1, queue);
        fflush(stdout);
        status = run_server() < 0;
        if (status)
            perror("服务端事件循环");
    }
    shutdown_server();
    return status;
}
