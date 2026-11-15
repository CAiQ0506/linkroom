"""Local account registration and password verification for Linkroom."""

from contextlib import closing
import hashlib
import hmac
import os
from pathlib import Path
import re
import secrets
import sqlite3
import sys

USERNAME = re.compile(r"[a-zA-Z0-9_-]{1,31}")
EMAIL_LOCAL = re.compile(r"[a-zA-Z0-9.!#$%&'*+/=?^_{}|~-]{1,64}", re.ASCII)
DOMAIN_LABEL = re.compile(r"[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?")
ITERATIONS = 600_000
SALT_SIZE = 16
PASSWORD_BYTES = 256


class DuplicateAccount(Exception):
    pass


def password_bytes(value):
    if not isinstance(value, str) or not 8 <= len(value) <= 64:
        raise ValueError("密码须为 8–64 个字符")
    if not all(32 <= ord(character) != 127 for character in value):
        raise ValueError("密码不能包含控制字符")
    try:
        encoded = value.encode("utf-8")
    except UnicodeError as error:
        raise ValueError("密码包含无效字符") from error
    if len(encoded) > PASSWORD_BYTES:
        raise ValueError("密码太长")
    return encoded


def normalize_email(value):
    email = value.strip() if isinstance(value, str) else ""
    local, _, domain = email.partition("@")
    labels = domain.split(".")
    if (len(email) > 254 or not EMAIL_LOCAL.fullmatch(local) or
            local.startswith(".") or local.endswith(".") or ".." in local or
            len(labels) < 2 or not all(DOMAIN_LABEL.fullmatch(label) for label in labels)):
        raise ValueError("请输入有效的邮箱地址")
    return email.lower()


def derive(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password, salt, ITERATIONS)


class Accounts:
    def __init__(self, data_dir):
        self.path = Path(data_dir).resolve() / "accounts.sqlite3"

    def initialize(self):
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        descriptor = os.open(self.path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        os.close(descriptor)
        with closing(sqlite3.connect(self.path)) as database, database:
            database.execute("""
                CREATE TABLE IF NOT EXISTS users (
                    username TEXT PRIMARY KEY NOT NULL,
                    email TEXT NOT NULL UNIQUE,
                    salt BLOB NOT NULL,
                    password_hash BLOB NOT NULL
                )
            """)

    def register(self, username, email, password):
        if not isinstance(username, str) or not USERNAME.fullmatch(username):
            raise ValueError("用户名应为 1–31 位英文字母、数字、下划线或连字符")
        address = normalize_email(email)
        secret = password_bytes(password)
        salt = secrets.token_bytes(SALT_SIZE)
        digest = derive(secret, salt)
        try:
            with closing(sqlite3.connect(self.path)) as database, database:
                database.execute("INSERT INTO users VALUES (?, ?, ?, ?)",
                                 (username, address, salt, digest))
        except sqlite3.IntegrityError as error:
            raise DuplicateAccount from error

    def verify(self, username, password):
        secret = password_bytes(password)
        with closing(sqlite3.connect(self.path.as_uri() + "?mode=ro", uri=True)) as database:
            row = database.execute("SELECT salt, password_hash FROM users WHERE username = ?",
                                   (username,)).fetchone()
        salt, digest = row if row else (bytes(SALT_SIZE), bytes(32))
        return hmac.compare_digest(digest, derive(secret, salt)) and row is not None


def verify_from_stdin():
    # The C login worker passes the password through a private pipe.
    data_dir, username = sys.argv[1:]
    try:
        password = sys.stdin.buffer.read(PASSWORD_BYTES + 1).decode("utf-8")
        return 0 if Accounts(data_dir).verify(username, password) else 1
    except ValueError:
        return 1
    except (OSError, sqlite3.Error):
        return 2


if __name__ == "__main__":
    sys.exit(verify_from_stdin())
