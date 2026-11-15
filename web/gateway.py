"""Serve the web client and translate its local HTTP calls to the C chat protocol."""

import argparse
from collections import deque
import hashlib
from http import HTTPStatus
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import socket
import sqlite3
import struct
import tempfile
import threading
import time
from urllib.parse import parse_qs, quote, urlsplit

from accounts import Accounts, DuplicateAccount, USERNAME, password_bytes

ROOT = Path(__file__).resolve().parent
HEADER = struct.Struct("!HHI")
MAX_FILE = 1024 ** 3
CLIENT_LIMIT = 1024
COOKIE = "linkroom_session"
ID = re.compile(r"[0-9a-f]{32}")
DIGEST = re.compile(r"[0-9a-f]{64}")
LOGIN, GROUP, PRIVATE, WHO, HISTORY, QUIT = range(1, 7)
WELCOME, EVENT, ERROR, END = range(20, 24)
UPLOAD, DOWNLOAD, READY, RESULT = range(40, 44)


class RequestError(Exception):
    def __init__(self, message, status=HTTPStatus.BAD_REQUEST):
        super().__init__(message)
        self.status = status


def exact(source, size):
    data = source.read(size)
    if len(data) != size:
        raise EOFError("服务端连接已关闭")
    return data


def frame(source):
    version, kind, length = HEADER.unpack(exact(source, HEADER.size))
    if version != 1 or length > 2048:
        raise ValueError("服务端消息帧格式无效")
    return kind, exact(source, length).decode("utf-8", errors="replace")


def send(sock, kind, text=""):
    data = text.encode("utf-8")
    if len(data) > CLIENT_LIMIT:
        raise RequestError("消息超过 1024 字节")
    sock.sendall(HEADER.pack(1, kind, len(data)) + data)


def expect(source, expected):
    kind, message = frame(source)
    if kind == ERROR:
        raise RequestError(message)
    if kind != expected:
        raise ValueError("服务端返回了意外的消息类型")
    return message


def plain(text):
    return isinstance(text, str) and text and all(32 <= ord(c) != 127 for c in text)


class ChatSession:
    def __init__(self, port, name, password):
        self.name = name
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.reader = self.socket.makefile("rb")
        try:
            send(self.socket, LOGIN, f"{name}\t{password}")
            self.file_token = expect(self.reader, WELCOME)
            if not ID.fullmatch(self.file_token):
                raise ValueError("无效的文件会话")
            self.socket.settimeout(None)
        except BaseException:
            self.reader.close()
            self.socket.close()
            raise

        self.condition = threading.Condition()
        self.tx = threading.Lock()
        self.sequence = 0
        self.events = deque(maxlen=250)
        self.last_seen = time.monotonic()
        self.closed = False
        threading.Thread(target=self.receive, daemon=True).start()

    def receive(self):
        try:
            while True:
                kind, text = frame(self.reader)
                with self.condition:
                    self.sequence += 1
                    self.events.append({"id": self.sequence, "kind": kind, "text": text})
                    self.condition.notify_all()
        except (EOFError, OSError, ValueError):
            self.close()
        finally:
            self.reader.close()

    def post(self, kind, text=""):
        with self.tx:
            if self.closed:
                raise RequestError("聊天连接已关闭", HTTPStatus.UNAUTHORIZED)
            send(self.socket, kind, text)

    def poll(self, after):
        with self.condition:
            self.condition.wait_for(lambda: self.closed or self.sequence > after, timeout=12)
            return {
                "events": [event for event in self.events if event["id"] > after],
                "closed": self.closed,
            }

    def snapshot(self):
        with self.condition:
            private = [event for event in self.events
                       if event["kind"] == EVENT and re.match(r"^\[[^\]]+\] \[私聊\] ", event["text"])]
            return {"name": self.name, "cursor": self.sequence, "private": private}

    def touch(self):
        with self.condition:
            self.last_seen = time.monotonic()

    def idle(self):
        with self.condition:
            return time.monotonic() - self.last_seen > 90

    def close(self):
        with self.condition:
            if self.closed:
                return
            self.closed = True
            self.condition.notify_all()
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()


class Gateway(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, chat_port, accounts):
        super().__init__(("127.0.0.1", port), Handler)
        self.chat_port = chat_port
        self.accounts = accounts
        self.sessions = {}
        self.sessions_lock = threading.Lock()

    def service_actions(self):
        with self.sessions_lock:
            expired = [key for key, session in self.sessions.items() if session.idle()]
            sessions = [self.sessions.pop(key) for key in expired]
        for session in sessions:
            session.close()

    def server_close(self):
        with self.sessions_lock:
            sessions = list(self.sessions.values())
            self.sessions.clear()
        for session in sessions:
            session.close()
        super().server_close()


class Handler(BaseHTTPRequestHandler):
    timeout = 30
    static = {
        "/": ("index.html", "text/html; charset=utf-8"),
        "/index.html": ("index.html", "text/html; charset=utf-8"),
        "/app.js": ("app.js", "text/javascript; charset=utf-8"),
        "/style.css": ("style.css", "text/css; charset=utf-8"),
        "/icon.svg": ("icon.svg", "image/svg+xml"),
        "/login-art.webp": ("login-art.webp", "image/webp"),
    }

    def do_GET(self):
        self.dispatch()

    def do_POST(self):
        self.dispatch()

    def log_request(self, code="-", size="-"):
        if str(code).startswith("5"):
            super().log_request(code, size)

    def respond(self, data, content_type, status=HTTPStatus.OK, cookie=None):
        self.started = True
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(data)

    def json(self, data, status=HTTPStatus.OK, cookie=None):
        self.respond(json.dumps(data, ensure_ascii=False).encode(),
                     "application/json; charset=utf-8", status, cookie)

    def payload(self, limit):
        length = self.headers.get("Content-Length", "")
        if not re.fullmatch(r"[0-9]{1,10}", length):
            raise RequestError("请求缺少有效的内容长度")
        size = int(length)
        if size > limit:
            raise RequestError("请求超过大小限制", HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        return size

    def read_json(self):
        data = json.loads(exact(self.rfile, self.payload(8192)))
        if not isinstance(data, dict):
            raise ValueError("请求格式无效")
        return data

    def get_session(self, required=True):
        try:
            cookie = SimpleCookie(self.headers.get("Cookie", "")).get(COOKIE)
        except CookieError:
            cookie = None
        key = cookie.value if cookie else None
        with self.server.sessions_lock:
            session = self.server.sessions.get(key)
            if session:
                session.touch()
        if not session or session.closed:
            if required:
                raise RequestError("请先连接聊天室", HTTPStatus.UNAUTHORIZED)
            return None, None
        return key, session

    def dispatch(self):
        self.started = False
        try:
            host = self.headers.get("Host", "")
            port = self.server.server_port
            hosts = {f"localhost:{port}", f"127.0.0.1:{port}"}
            if host not in hosts:
                raise RequestError("请从本机地址访问网页", HTTPStatus.FORBIDDEN)
            if self.command == "POST" and self.headers.get("Origin", f"http://{host}") != f"http://{host}":
                raise RequestError("无效的请求来源", HTTPStatus.FORBIDDEN)
            if self.headers.get("Sec-Fetch-Site") == "cross-site":
                raise RequestError("请从本机网页发起请求", HTTPStatus.FORBIDDEN)

            path = urlsplit(self.path)
            if self.command == "GET" and path.path in self.static:
                name, kind = self.static[path.path]
                self.respond((ROOT / name).read_bytes(), kind)
            elif path.path == "/api/session":
                self.session_route()
            elif self.command == "POST" and path.path == "/api/register":
                self.register_route()
            elif self.command == "POST" and path.path == "/api/logout":
                key, session = self.get_session()
                with self.server.sessions_lock:
                    self.server.sessions.pop(key, None)
                session.close()
                self.json({}, cookie=f"{COOKIE}=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict")
            elif self.command == "GET" and path.path == "/api/events":
                _, session = self.get_session()
                after = parse_qs(path.query).get("after", ["0"])[0]
                if not re.fullmatch(r"[0-9]{1,12}", after):
                    raise ValueError("消息位置无效")
                self.json(session.poll(int(after)))
            elif self.command == "POST" and path.path == "/api/chat":
                _, session = self.get_session()
                self.chat_route(session)
            elif self.command == "POST" and path.path == "/api/upload":
                _, session = self.get_session()
                self.upload(session, parse_qs(path.query).get("name", [""])[0])
            elif self.command == "GET" and path.path.startswith("/api/files/"):
                _, session = self.get_session()
                self.download(session, path.path.removeprefix("/api/files/"))
            else:
                raise RequestError("地址不存在", HTTPStatus.NOT_FOUND)
        except RequestError as error:
            if not self.started:
                self.json({"error": str(error)}, error.status)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except ValueError as error:
            if not self.started:
                self.json({"error": str(error)}, HTTPStatus.BAD_REQUEST)
        except sqlite3.Error:
            if not self.started:
                self.json({"error": "账号暂时无法保存，请稍后重试"}, HTTPStatus.SERVICE_UNAVAILABLE)
        except (OSError, EOFError) as error:
            if not self.started:
                self.json({"error": f"无法连接或读取 C 服务端。请先运行 ./server。({error})"},
                          HTTPStatus.BAD_GATEWAY)

    def session_route(self):
        _, session = self.get_session(required=False)
        if self.command == "POST":
            if session:
                raise RequestError("请先退出当前账号再重新登录", HTTPStatus.CONFLICT)
            credentials = self.read_json()
            name, password = credentials.get("name"), credentials.get("password")
            if not isinstance(name, str) or not USERNAME.fullmatch(name):
                raise RequestError("请输入有效的用户名")
            password_bytes(password)
            session = ChatSession(self.server.chat_port, name, password)
            key = secrets.token_hex(24)
            with self.server.sessions_lock:
                self.server.sessions[key] = session
            self.json(session.snapshot(), cookie=f"{COOKIE}={key}; Path=/; HttpOnly; SameSite=Strict")
        else:
            self.json(session.snapshot() if session else {"name": None})

    def register_route(self):
        fields = self.read_json()
        try:
            self.server.accounts.register(fields.get("name"), fields.get("email"), fields.get("password"))
        except DuplicateAccount as error:
            raise RequestError("用户名或邮箱已注册", HTTPStatus.CONFLICT) from error
        self.json({"name": fields["name"]}, HTTPStatus.CREATED)

    def chat_route(self, session):
        data = self.read_json()
        action = data.get("action")
        text = data.get("text", "")
        if action in ("group", "private"):
            if not plain(text):
                raise RequestError("消息为空或包含控制字符")
            if action == "private":
                user = data.get("user")
                if not isinstance(user, str) or not USERNAME.fullmatch(user):
                    raise RequestError("私聊对象无效")
                session.post(PRIVATE, f"{user} {text}")
            else:
                session.post(GROUP, text)
        elif action == "who":
            session.post(WHO)
        elif action == "history":
            session.post(HISTORY, "20")
        else:
            raise RequestError("未知的聊天操作")
        self.json({})

    def file_connection(self):
        return socket.create_connection(("127.0.0.1", self.server.chat_port + 1), timeout=30)

    def upload(self, session, name):
        size = self.payload(MAX_FILE)
        if not plain(name) or len(name.encode()) > 200 or name in (".", "..") or "/" in name or "\\" in name:
            raise RequestError("请选择有效的文件名，文件名最多 200 字节")
        digest = hashlib.sha256()
        # The C service needs the checksum before it receives the first byte.
        with tempfile.TemporaryFile() as data:
            remaining = size
            while remaining:
                chunk = exact(self.rfile, min(65536, remaining))
                digest.update(chunk)
                data.write(chunk)
                remaining -= len(chunk)
            data.seek(0)
            with self.file_connection() as upstream, upstream.makefile("rb") as source:
                send(upstream, UPLOAD, f"{session.file_token}\t{size}\t{digest.hexdigest()}\t{name}")
                file_id = expect(source, READY)
                while chunk := data.read(65536):
                    upstream.sendall(chunk)
                if expect(source, RESULT) != file_id:
                    raise ValueError("文件编号不一致")
        self.json({"id": file_id})

    def download(self, session, file_id):
        if not ID.fullmatch(file_id):
            raise RequestError("文件编号无效")
        with self.file_connection() as upstream, upstream.makefile("rb") as source:
            send(upstream, DOWNLOAD, f"{session.file_token}\t{file_id}\t0")
            size_text, expected, filename = expect(source, READY).split("\t", 2)
            if not size_text.isdigit() or int(size_text) > MAX_FILE or not DIGEST.fullmatch(expected):
                raise ValueError("文件元数据无效")
            size = int(size_text)
            digest = hashlib.sha256()
            # Verify the payload before offering it as a completed browser download.
            with tempfile.TemporaryFile() as data:
                remaining = size
                while remaining:
                    chunk = exact(source, min(65536, remaining))
                    digest.update(chunk)
                    data.write(chunk)
                    remaining -= len(chunk)
                if expect(source, RESULT) != file_id or digest.hexdigest() != expected:
                    raise RequestError("下载 SHA-256 校验失败")
                data.seek(0)
                self.started = True
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Content-Disposition", "attachment; filename*=UTF-8''" + quote(filename, safe=""))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                while chunk := data.read(65536):
                    self.wfile.write(chunk)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080, help="web port")
    parser.add_argument("--chat-port", type=int, default=9000, help="C chat port")
    parser.add_argument("--data-dir", type=Path, default=ROOT.parent / "chat-data",
                        help="same data directory as the C server")
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not 1 <= args.chat_port < 65535:
        parser.error("invalid port")
    os.umask(0o077)
    accounts = Accounts(args.data_dir)
    accounts.initialize()
    with Gateway(args.port, args.chat_port, accounts) as gateway:
        print(f"网页聊天室 http://127.0.0.1:{args.port}/", flush=True)
        try:
            gateway.serve_forever()
        except KeyboardInterrupt:
            pass
