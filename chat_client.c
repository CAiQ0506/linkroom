#include "protocol.h"
#include "transfer.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct Client {
    int socket;
    const char *address;
    uint16_t file_port;
    char token[ID_HEX + 1];
    pid_t transfer;
    int quitting;
};

static void help(void)
{
    puts("直接输入文字发送群聊");
    puts("/msg <用户名> <消息>       发送私聊");
    puts("/who                      查看在线用户");
    puts("/history [1–20]           查看最近的群聊和群文件记录");
    puts("/upload <本地路径>        上传群文件");
    puts("/get <文件ID> <本地路径>  下载或继续下载");
    puts("/help                     查看命令");
    puts("/quit                     退出聊天");
}

static void reap_transfer(struct Client *client)
{
    if (client->transfer && waitpid(client->transfer, NULL, WNOHANG) == client->transfer)
        client->transfer = 0;
}

static void start_transfer(struct Client *client, const char *id, const char *path)
{
    reap_transfer(client);
    if (client->transfer) {
        puts("有一个文件任务正在运行");
        return;
    }
    pid_t child = fork();
    if (!child) {
        close(client->socket);
        int result = id ? download_file(client->address, client->file_port, client->token, id, path)
                        : upload_file(client->address, client->file_port, client->token, path);
        _exit(result == 0 ? 0 : 1);
    }
    if (child < 0)
        perror("创建文件传输进程");
    else
        client->transfer = child;
}

static int quit_chat(struct Client *client)
{
    client->quitting = 1;
    return send_frame(client->socket, C_QUIT, "");
}

static int handle_input(struct Client *client, char *line)
{
    if (!*line)
        return 0;
    if (!valid_text(line)) {
        puts("输入含有控制字符");
        return 0;
    }
    if (!strcmp(line, "/quit"))
        return quit_chat(client);
    if (!strcmp(line, "/help")) {
        help();
        return 0;
    }
    if (!strcmp(line, "/who"))
        return send_frame(client->socket, C_WHO, "");
    if (!strcmp(line, "/history"))
        return send_frame(client->socket, C_HISTORY, "");
    if (!strncmp(line, "/history ", 9))
        return send_frame(client->socket, C_HISTORY, line + 9);
    if (!strncmp(line, "/msg ", 5))
        return send_frame(client->socket, C_PRIVATE, line + 5);
    if (!strncmp(line, "/upload ", 8) && line[8]) {
        start_transfer(client, NULL, line + 8);
        return 0;
    }
    if (!strncmp(line, "/get ", 5)) {
        char *id = line + 5;
        char *path = strchr(id, ' ');
        if (path) {
            *path++ = '\0';
            if (*path && valid_hex(id, ID_HEX)) {
                start_transfer(client, id, path);
                return 0;
            }
        }
        puts("用法 /get <文件ID> <本地路径>");
        return 0;
    }
    if (line[0] == '/') {
        puts("未知命令或参数不完整，输入 /help 查看用法");
        return 0;
    }
    return send_frame(client->socket, C_GROUP, line);
}

static int chat_loop(struct Client *client)
{
    char line[CHAT_INPUT + 1];
    size_t used = 0;
    int discard = 0;

    for (;;) {
        struct pollfd events[] = {
            { .fd = client->socket, .events = POLLIN },
            { .fd = client->quitting ? -1 : STDIN_FILENO, .events = POLLIN }
        };
        int timeout = client->quitting ? 3000 : client->transfer ? 250 : -1;
        int ready = poll(events, 2, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (client->quitting && !ready)
            return 0;
        reap_transfer(client);
        if (events[0].revents) {
            char text[CHAT_TEXT + 1];
            uint16_t type;
            int result = recv_frame(client->socket, &type, text, sizeof(text));
            if (result <= 0) {
                if (!client->quitting)
                    puts("聊天连接已关闭");
                return result < 0 ? -1 : 0;
            }
            if (type == S_ERROR)
                printf("错误：%s\n", text);
            else if (type == S_EVENT || type == S_END)
                puts(text);
            if (type == S_END && client->quitting && !strcmp(text, "已退出"))
                return 0;
        }

        if (!events[1].revents)
            continue;
        char input[2048];
        ssize_t count = read(STDIN_FILENO, input, sizeof(input));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (!count) {
            if (!discard && used) {
                line[used] = '\0';
                if (handle_input(client, line) < 0)
                    return -1;
            }
            if (!client->quitting && quit_chat(client) < 0)
                return -1;
            continue;
        }

        for (ssize_t i = 0; i < count && !client->quitting; ++i) {
            char byte = input[i];
            if (byte == '\n') {
                if (discard) {
                    puts("输入无效或超过 1024 字节");
                } else {
                    if (used && line[used - 1] == '\r')
                        --used;
                    line[used] = '\0';
                    if (handle_input(client, line) < 0)
                        return -1;
                }
                used = 0;
                discard = 0;
            } else if (!discard) {
                if (used == CHAT_INPUT || byte == '\0')
                    discard = 1;
                else
                    line[used++] = byte;
            }
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t port;
    if (argc != 4 || !parse_number(argv[2], 65534, &port) || !port || !valid_name(argv[3])) {
        fprintf(stderr, "用法 %s <IPv4地址> <聊天端口> <用户名>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct Client client = { .address = argv[1], .file_port = (uint16_t)(port + 1) };
    client.socket = tcp_connect(client.address, (uint16_t)port);
    if (client.socket < 0) {
        perror("连接服务器");
        return 1;
    }
    socket_timeout(client.socket, 10);
    char password_line[PASSWORD_LIMIT + 3];
    char *password;
    if (isatty(STDIN_FILENO)) {
        password = getpass("密码：");
    } else {
        /* Allow scripted entry without buffering subsequent chat commands. */
        setvbuf(stdin, NULL, _IONBF, 0);
        password = fgets(password_line, sizeof(password_line), stdin);
        if (password)
            password[strcspn(password, "\r\n")] = '\0';
    }
    int sent = -1;
    if (password && valid_text(password) && strlen(password) <= PASSWORD_LIMIT) {
        char login[NAME_LIMIT + PASSWORD_LIMIT + 2];
        snprintf(login, sizeof(login), "%s\t%s", argv[3], password);
        sent = send_frame(client.socket, C_LOGIN, login);
        explicit_bzero(login, sizeof(login));
    } else {
        fprintf(stderr, "请输入有效的密码\n");
    }
    if (password)
        explicit_bzero(password, strlen(password));
    explicit_bzero(password_line, sizeof(password_line));

    char response[CHAT_TEXT + 1];
    uint16_t type;
    if (sent < 0 || recv_frame(client.socket, &type, response, sizeof(response)) != 1) {
        fprintf(stderr, "服务器未完成登录握手\n");
        close(client.socket);
        return 1;
    }
    if (type != S_WELCOME || !valid_hex(response, ID_HEX)) {
        fprintf(stderr, "%s\n", type == S_ERROR ? response : "登录响应无效");
        close(client.socket);
        return 1;
    }
    memcpy(client.token, response, sizeof(client.token));
    socket_timeout(client.socket, 0);
    puts("登录成功，输入 /help 查看命令");

    int result = chat_loop(&client);
    if (result < 0)
        perror("聊天连接");
    close(client.socket);
    if (client.transfer) {
        kill(client.transfer, SIGTERM);
        while (waitpid(client.transfer, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    return result < 0 ? 1 : 0;
}
