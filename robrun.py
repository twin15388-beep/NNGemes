"""NZL Studio — локальный сервер + запуск RobRun.exe (один файл).

Архитектура:
  - LocalServer (127.0.0.1:800): заменяет удалённый сервер 95.85.230.17.
    Эндпоинты: POST /api/v1/runtime/redeem, GET/PUT /api/v1/runtime/config,
    POST /api/v1/runtime/crash. Всё остальное (включая /api/v1/anticheat/*)
    возвращает 404 — RobRun.exe это терпит (проверено по requests.jsonl:
    9 проб /anticheat/status -> 404, клиент продолжает работу).
  - NamedPipe (\\\\.\\pipe\\RobRun.<32hex>): bootstrap-канал лаунчера.
    Лаунчер пишет ROBRUN/1\\n<LOCAL_URL>\\n1\\n<ticket>\\n, клиент отвечает
    initializing / waiting_game / ready.
  - ShellProcess: запуск RobRun.exe через ShellExecuteExW с
    --loader-pipe <имя канала>.

Запуск:
  python robrun.py                          # сервер + запуск RobRun.exe
  python robrun.py --server-only            # только локальный API
  python robrun.py --port 801 --data ./data # сервер на другом порту
  python robrun.py --help
"""

import argparse
import ctypes
import hashlib
import http.client
import json
import os
import re
import secrets
import struct
import sys
import tempfile
import threading
import time
import uuid
from ctypes import wintypes as w
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ORIGINAL_SHA256 = "7f1b07f7ba4584e41b4a02fc7be6bc3ec0e8766dddcef15dbd174d01bc576e01"
ORIGINAL_URL = "https://95.85.230.17"
LOCAL_URL = "http://127.0.0.1:800"
LEGACY_PATCHED_SHA256 = "983d8f61197dadf98b74ea9342576405885f6d6b956186aa8f1afaadfa718694"
ORIGINAL_SHA256_1089 = "4fdbec0c4c2bfddeb91870b1890148953ec32cb15f7e4cb5e095b989a46cbe8e"
PATCHED_SHA256_1089 = "6ca52c0a7c772bba6c5fdad1a3cde905305939471a61e622b4b75331e87cf486"
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
MAX_FRAME = 4096

# --- Windows API (только на nt; на остальных ОС имена не определены) ---
if os.name == "nt":
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    shell = ctypes.WinDLL("shell32", use_last_error=True)

    def declare(dll, name, result, *arguments):
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = result, list(arguments)
        return fn

    class Overlapped(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                    ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]

    class SecurityAttributes(ctypes.Structure):
        _fields_ = [("nLength", w.DWORD), ("lpSecurityDescriptor", ctypes.c_void_p),
                    ("bInheritHandle", w.BOOL)]

    class ShellExecuteInfo(ctypes.Structure):
        _fields_ = [("cbSize", w.DWORD), ("fMask", w.ULONG), ("hwnd", w.HWND),
                    ("lpVerb", w.LPCWSTR), ("lpFile", w.LPCWSTR),
                    ("lpParameters", w.LPCWSTR), ("lpDirectory", w.LPCWSTR),
                    ("nShow", ctypes.c_int), ("hInstApp", w.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", w.LPCWSTR),
                    ("hkeyClass", w.HKEY), ("dwHotKey", w.DWORD),
                    ("hIcon", w.HANDLE), ("hProcess", w.HANDLE)]

    POVERLAPPED = ctypes.POINTER(Overlapped)
    declare(k32, "CreateNamedPipeW", w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD,
            w.DWORD, w.DWORD, w.DWORD, w.DWORD, ctypes.POINTER(SecurityAttributes))
    declare(k32, "CreateFileW", w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD,
            ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE)
    declare(k32, "ConnectNamedPipe", w.BOOL, w.HANDLE, POVERLAPPED)
    declare(k32, "DisconnectNamedPipe", w.BOOL, w.HANDLE)
    declare(k32, "ReadFile", w.BOOL, w.HANDLE, ctypes.c_void_p, w.DWORD,
            ctypes.POINTER(w.DWORD), POVERLAPPED)
    declare(k32, "WriteFile", w.BOOL, w.HANDLE, ctypes.c_void_p, w.DWORD,
            ctypes.POINTER(w.DWORD), POVERLAPPED)
    declare(k32, "CancelIoEx", w.BOOL, w.HANDLE, POVERLAPPED)
    declare(k32, "GetOverlappedResult", w.BOOL, w.HANDLE, POVERLAPPED,
            ctypes.POINTER(w.DWORD), w.BOOL)
    declare(k32, "CreateEventW", w.HANDLE, ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR)
    declare(k32, "WaitForSingleObject", w.DWORD, w.HANDLE, w.DWORD)
    declare(k32, "CloseHandle", w.BOOL, w.HANDLE)
    declare(k32, "GetNamedPipeClientProcessId", w.BOOL, w.HANDLE, ctypes.POINTER(w.ULONG))
    declare(k32, "GetCurrentProcess", w.HANDLE)
    declare(k32, "GetProcessId", w.DWORD, w.HANDLE)
    declare(k32, "GetExitCodeProcess", w.BOOL, w.HANDLE, ctypes.POINTER(w.DWORD))
    declare(k32, "GetStdHandle", w.HANDLE, w.DWORD)
    declare(k32, "GetConsoleMode", w.BOOL, w.HANDLE, ctypes.POINTER(w.DWORD))
    declare(k32, "SetConsoleMode", w.BOOL, w.HANDLE, w.DWORD)
    declare(k32, "LocalFree", ctypes.c_void_p, ctypes.c_void_p)
    declare(adv, "OpenProcessToken", w.BOOL, w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE))
    declare(adv, "GetTokenInformation", w.BOOL, w.HANDLE, ctypes.c_int,
            ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD))
    declare(adv, "ConvertSidToStringSidW", w.BOOL, ctypes.c_void_p, ctypes.POINTER(w.LPWSTR))
    declare(adv, "ConvertStringSecurityDescriptorToSecurityDescriptorW", w.BOOL,
            w.LPCWSTR, w.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(w.DWORD))
    declare(shell, "ShellExecuteExW", w.BOOL, ctypes.POINTER(ShellExecuteInfo))


def configure_output():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    if os.name == "nt":
        try:
            handle = k32.GetStdHandle(-11)
            mode = w.DWORD()
            if handle and k32.GetConsoleMode(handle, ctypes.byref(mode)):
                k32.SetConsoleMode(handle, mode.value | 0x0004)
        except Exception:
            pass


def bootstrap(ticket):
    if not HEX64.fullmatch(ticket):
        raise ValueError("Билет должен содержать 64 строчных hex-символа")
    return f"ROBRUN/1\n{LOCAL_URL}\n1\n{ticket}\n".encode("utf-8")


def frame(payload):
    if not 0 < len(payload) <= MAX_FRAME:
        raise ValueError("Недопустимый размер пакета канала")
    return struct.pack("<I", len(payload)) + payload


def patched_image(original):
    """Патч RobRun 1.0.81: замена встроенного URL на локальный.

    ВНИМАНИЕ: применим только к сборке 1.0.81 (ORIGINAL_SHA256), где URL
    хранится как NUL-терминированная строка. В сборке 1089 URL собирается
    в рантайме из фрагментов "https://" / "http://" / ":%u" и plain-патч
    не работает — используйте готовый PATCHED_SHA256_1089.
    Функция оставлена для совместимости с 1.0.81, лаунчером не вызывается.
    """
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Исходный EXE отличается от исследованного RobRun 1.0.81")
    old, new = ORIGINAL_URL.encode("ascii"), LOCAL_URL.encode("ascii")
    if len(old) != len(new) or original.count(old + b"\0") != 1:
        raise ValueError("Встроенный адрес не соответствует ожидаемой структуре")
    offset = original.index(old + b"\0")
    result = original[:offset] + new + original[offset + len(old):]
    return result, offset


def verify_local_image(data):
    """Проверка, что EXE — подготовленная локальная копия.

    Основной путь — SHA-allowlist известных патченных сборок (1089 и legacy).
    Запасной путь — структурная проверка для патча 1.0.81 (plain-строка URL).
    Для сборки 1089 запасной путь неприменим (URL runtime-constructed),
    поэтому неподходящий SHA всегда отклоняется с понятным сообщением.
    """
    digest = hashlib.sha256(data).hexdigest()
    if digest in (PATCHED_SHA256_1089, LEGACY_PATCHED_SHA256):
        return
    old, new = ORIGINAL_URL.encode("ascii"), LOCAL_URL.encode("ascii")
    if data.count(new + b"\0") == 1:
        offset = data.index(new + b"\0")
        restored = data[:offset] + old + data[offset + len(new):]
        if hashlib.sha256(restored).hexdigest() == ORIGINAL_SHA256:
            return
        raise ValueError("Локальная копия содержит дополнительные изменения")
    raise ValueError(
        "EXE не является подготовленной локальной копией "
        f"(sha256={digest[:16]}…, ожидался {PATCHED_SHA256_1089[:16]}…)"
    )


CONFIG_LIMIT = 16 * 1024
CRASH_LIMIT = 64 * 1024


class State:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.config_file = self.directory / "config.ini"
        self.crash_directory = self.directory / "crashes"
        self.crash_directory.mkdir(exist_ok=True)
        self.tokens = set()
        self.lock = threading.RLock()

    def issue_token(self):
        token = secrets.token_hex(32)
        with self.lock:
            self.tokens.add(token)
        return token

    def authorized(self, header):
        if not header or not header.startswith("Bearer "):
            return False
        with self.lock:
            return header[7:] in self.tokens

    def read_config(self):
        with self.lock:
            return self.config_file.read_bytes() if self.config_file.exists() else b""

    def write_config(self, content):
        with self.lock:
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=self.directory, delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(content)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, self.config_file)
            finally:
                if temporary is not None and temporary.exists():
                    temporary.unlink()

    def save_crash(self, report):
        name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        path = self.crash_directory / f"{name}-{uuid.uuid4().hex}.json"
        with path.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")

    def log(self, method, path, size, status):
        event = dict(time=datetime.now(timezone.utc).isoformat(), method=method,
                     path=path, bytes=size, status=status)
        line = json.dumps(event, ensure_ascii=True)
        with self.lock:
            with (self.directory / "requests.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(line + "\n")


def validate_config(data):
    text = data.decode("utf-8-sig", errors="strict")
    if "\0" in text:
        raise ValueError("NUL is not permitted")
    for line in text.split("\n"):
        line = line.strip(" \t\r")
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            continue
        if "=" not in line:
            raise ValueError("Expected an INI entry")


class RequestError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "RobRunLocal/1"

    def log_message(self, *_):
        pass  # The metadata-only log below does not include tickets or tokens.

    def reply(self, status, body=b"", content_type="text/plain; charset=utf-8"):
        self.close_connection = True
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def body(self, limit):
        if self.headers.get("Transfer-Encoding"):
            raise RequestError(400, "Chunked bodies are unsupported")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            raise RequestError(400, "Expected Content-Length")
        size = int(lengths[0])
        self.request_size = size
        if size > limit:
            raise RequestError(413, "Body too large")
        data = self.rfile.read(size)
        if len(data) != size:
            raise RequestError(400, "Incomplete body")
        return data

    def dispatch(self):
        self.connection.settimeout(5)
        self.request_size = 0
        path = urlsplit(self.path).path
        routes = {"/api/v1/runtime/redeem": {"POST"},
                  "/api/v1/runtime/config": {"GET", "PUT"},
                  "/api/v1/runtime/crash": {"POST"}}
        status = 500
        state = self.server.state
        try:
            if path not in routes:
                raise RequestError(404, "Unknown endpoint")
            if self.command not in routes[path]:
                raise RequestError(405, "Method not allowed")
            if path.endswith("/redeem"):
                ticket = self.body(64).decode("ascii")
                if not HEX64.fullmatch(ticket):
                    raise RequestError(400, "Expected a 64-character lowercase hex ticket")
                status = 200
                self.reply(status, state.issue_token().encode("ascii"))
            else:
                auth = self.headers.get_all("Authorization", [])
                if len(auth) != 1 or not state.authorized(auth[0]):
                    raise RequestError(401, "Unknown local token; redeem a ticket again")
                if path.endswith("/config") and self.command == "GET":
                    data = state.read_config()
                    if len(data) > CONFIG_LIMIT:
                        raise RequestError(500, "Stored configuration is too large")
                    validate_config(data)
                    status = 200
                    self.reply(status, data)
                elif path.endswith("/config"):
                    data = self.body(CONFIG_LIMIT)
                    validate_config(data)
                    state.write_config(data)
                    status = 204
                    self.reply(status)
                else:
                    data = self.body(CRASH_LIMIT)

                    def reject_constant(_):
                        raise ValueError("Non-finite JSON number")

                    report = json.loads(data.decode("utf-8"), parse_constant=reject_constant)
                    if not isinstance(report, dict):
                        raise RequestError(400, "Expected a JSON object")
                    state.save_crash(report)
                    status = 204
                    self.reply(status)
        except RequestError as error:
            status = error.status
            self.reply(status, error.message.encode("utf-8"))
        except (UnicodeError, ValueError):
            status = 400
            self.reply(status, b"Invalid body or configuration format")
        except (TimeoutError, ConnectionError):
            status = 408
            try:
                self.reply(status, b"Request timed out or connection closed")
            except OSError:
                pass
        except OSError:
            status = 500
            try:
                self.reply(status, b"Local storage or connection error")
            except OSError:
                pass
        finally:
            try:
                state.log(self.command, path, self.request_size, status)
            except OSError as error:
                print(f"Cannot write request log: {error}", flush=True)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = do_PATCH = do_OPTIONS = dispatch


class LocalServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, port, directory):
        self.state = State(directory)
        super().__init__(("127.0.0.1", port), Handler)


def require_windows():
    if os.name != "nt":
        raise OSError("Именованный канал клиента поддерживается только в Windows")


def current_sid():
    require_windows()
    token, size, sid_text = w.HANDLE(), w.DWORD(), w.LPWSTR()
    if not adv.OpenProcessToken(k32.GetCurrentProcess(), 8, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        adv.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
        buffer = ctypes.create_string_buffer(size.value)
        if not adv.GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        sid = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p))[0]
        if not adv.ConvertSidToStringSidW(sid, ctypes.byref(sid_text)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return ctypes.wstring_at(sid_text)
        finally:
            k32.LocalFree(ctypes.cast(sid_text, ctypes.c_void_p))
    finally:
        k32.CloseHandle(token)


def pipe_path(name):
    if not re.fullmatch(r"RobRun\.[0-9a-f]{32}", name):
        raise ValueError("Недопустимое имя канала")
    return "\\\\.\\pipe\\" + name


class NamedPipe:
    def __init__(self, name):
        require_windows()
        self.handle = None
        self.server_side = True
        descriptor = ctypes.c_void_p()
        sddl = f"D:P(A;;GA;;;{current_sid()})(A;;GA;;;SY)"
        if not adv.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, 1, ctypes.byref(descriptor), None):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            security = SecurityAttributes(ctypes.sizeof(SecurityAttributes), descriptor, False)
            handle = k32.CreateNamedPipeW(pipe_path(name), 3 | 0x40000000 | 0x80000,
                                         8, 1, 8192, 8192, 0, ctypes.byref(security))
            if handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            self.handle = handle
        finally:
            k32.LocalFree(descriptor)

    @classmethod
    def open_client(cls, name):
        """Клиентская сторона канала. Используется тестами транспорта."""
        require_windows()
        handle = k32.CreateFileW(pipe_path(name), 0xC0000000, 0, None, 3, 0x40000000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        result = cls.__new__(cls)
        result.handle, result.server_side = handle, False
        return result

    def operation(self, function, timeout, buffer=None, size=0, alive=None):
        event = k32.CreateEventW(None, True, False, None)
        if not event:
            raise ctypes.WinError(ctypes.get_last_error())
        overlapped = Overlapped()
        overlapped.hEvent = event
        transferred = w.DWORD()
        pending = False
        try:
            if buffer is None:
                succeeded = function(self.handle, ctypes.byref(overlapped))
            else:
                succeeded = function(self.handle, buffer, size,
                                     ctypes.byref(transferred), ctypes.byref(overlapped))
            if succeeded:
                return transferred.value
            error = ctypes.get_last_error()
            if buffer is None and error == 535:  # ERROR_PIPE_CONNECTED
                return 0
            if error in (109, 232, 233):
                raise EOFError("Клиент закрыл канал")
            if error != 997:  # ERROR_IO_PENDING
                raise ctypes.WinError(error)
            pending = True
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("Истекло время ожидания канала")
                if alive is not None and not alive():
                    raise EOFError("Процесс клиента завершился")
                result = k32.WaitForSingleObject(event, max(1, min(250, int(remaining * 1000))))
                if result == 0:
                    break
                if result != 258:
                    raise ctypes.WinError(ctypes.get_last_error())
            pending = False
            if not k32.GetOverlappedResult(self.handle, ctypes.byref(overlapped),
                                         ctypes.byref(transferred), False):
                error = ctypes.get_last_error()
                if error in (109, 232, 233):
                    raise EOFError("Клиент закрыл канал")
                raise ctypes.WinError(error)
            return transferred.value
        finally:
            if pending:
                k32.CancelIoEx(self.handle, ctypes.byref(overlapped))
                k32.GetOverlappedResult(self.handle, ctypes.byref(overlapped),
                                       ctypes.byref(transferred), True)
            k32.CloseHandle(event)

    def connect(self, timeout=15, alive=None):
        self.operation(k32.ConnectNamedPipe, timeout, alive=alive)

    def client_pid(self):
        pid = w.ULONG()
        if not k32.GetNamedPipeClientProcessId(self.handle, ctypes.byref(pid)):
            raise ctypes.WinError(ctypes.get_last_error())
        return pid.value

    def read_exact(self, size, timeout=15, alive=None):
        chunks, deadline = [], time.monotonic() + timeout
        while size:
            buffer = ctypes.create_string_buffer(size)
            count = self.operation(k32.ReadFile, deadline - time.monotonic(),
                                   buffer, size, alive)
            if count == 0:
                raise EOFError("Канал закрылся до конца пакета")
            chunks.append(buffer.raw[:count])
            size -= count
        return b"".join(chunks)

    def read_frame(self, timeout=15, alive=None):
        deadline = time.monotonic() + timeout
        size = struct.unpack("<I", self.read_exact(4, timeout, alive))[0]
        if not 0 < size <= MAX_FRAME:
            raise ValueError("Клиент передал недопустимый размер пакета")
        return self.read_exact(size, deadline - time.monotonic(), alive)

    def write_frame(self, payload, timeout=15):
        content, deadline = frame(payload), time.monotonic() + timeout
        while content:
            buffer = ctypes.create_string_buffer(content)
            count = self.operation(k32.WriteFile, deadline - time.monotonic(), buffer, len(content))
            if not count:
                raise EOFError("Не удалось записать пакет в канал")
            content = content[count:]

    def close(self):
        if self.handle is not None:
            if self.server_side:
                k32.DisconnectNamedPipe(self.handle)
            k32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class ShellProcess:
    """Запуск RobRun.exe через ShellExecuteExW (только Windows)."""
    def __init__(self, executable, pipe_name):
        require_windows()
        info = ShellExecuteInfo()
        info.cbSize, info.fMask = ctypes.sizeof(info), 0x40 | 0x100
        info.lpVerb, info.lpFile = "open", str(executable)
        info.lpParameters = "--loader-pipe " + pipe_name
        info.lpDirectory, info.nShow = str(executable.parent), 1
        if not shell.ShellExecuteExW(ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not info.hProcess:
            raise OSError("Windows не вернула процесс клиента")
        self.handle = info.hProcess
        self.pid = k32.GetProcessId(self.handle)
        if not self.pid:
            self.close()
            raise ctypes.WinError(ctypes.get_last_error())

    def poll(self):
        code = w.DWORD()
        if not k32.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return None if code.value == 259 else code.value

    def wait(self):
        while k32.WaitForSingleObject(self.handle, 500) == 258:
            pass
        return self.poll()

    def close(self):
        if self.handle:
            k32.CloseHandle(self.handle)
            self.handle = None


def check_server(port=800):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request("HEAD", "/")
        response = connection.getresponse()
        if not response.getheader("Server", "").startswith("RobRunLocal/1"):
            raise OSError(f"На порту {port} отвечает другой сервер")
        response.read()
    finally:
        connection.close()


def _print_donation_box():
    print(
        "\n"
        "\033[35m    $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n"
        "\033[95m    $                                                               $\n"
        "\033[95m    $   NZL Studio                                                  $\n"
        "\033[95m    $                                                               $\n"
        "\033[95m    $   Поддержать проект                                           $\n"
        "\033[95m    $   для скорейшего выпуска новых кряков                         $\n"
        "\033[95m    $                                                               $\n"
        "\033[95m    $   TRtMKz8R4dzetkCkTZVkZdsE6zdYKrBkRr                          $\n"
        "\033[95m    $   USDT    TRC20    TRON                                       $\n"
        "\033[95m    $                                                               $\n"
        "\033[35m    $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n"
        "\033[0m",
        flush=True,
    )


def launch_robrun(executable: Path, port=800):
    configure_output()
    executable = Path(executable).resolve()
    verify_local_image(executable.read_bytes())
    check_server(port)
    payload = bootstrap(secrets.token_hex(32))
    name = "RobRun." + uuid.uuid4().hex

    stop_anim = threading.Event()
    status_state = {"mode": "preparing", "shown_wait": False}
    state_lock = threading.Lock()

    def animate():
        tick = 0
        while not stop_anim.is_set():
            with state_lock:
                mode = status_state["mode"]
                shown_wait = status_state["shown_wait"]
                if mode == "waiting_game" and not shown_wait:
                    status_state["shown_wait"] = True
                    print("\n\033[95m    [NZL Studio] please start the game\033[0m", flush=True)
            if mode == "preparing":
                tick = (tick % 16) + 1
                bar = ("$" * tick) + ("." * (16 - tick))
                print(f"\r\033[2K\033[95m    [NZL Studio] preparing  {bar}\033[0m", end="", flush=True)
            stop_anim.wait(0.25)

    anim_thread = threading.Thread(target=animate, daemon=True)
    anim_thread.start()

    try:
        with NamedPipe(name) as pipe:
            process = ShellProcess(executable, name)
            try:
                alive = lambda: process.poll() is None
                pipe.connect(timeout=15, alive=alive)
                if pipe.client_pid() != process.pid:
                    raise OSError("К каналу подключился другой процесс")
                pipe.write_frame(payload)
                payload = None
                while True:
                    message = pipe.read_frame(timeout=300, alive=alive).decode("utf-8")
                    if message == "ready":
                        stop_anim.set()
                        anim_thread.join(timeout=1)
                        with state_lock:
                            prefix = "" if status_state["shown_wait"] else "\n"
                        print(f"{prefix}\033[95m    [NZL Studio] ready      $$$$$$$$$$$$$$$$\033[0m", flush=True)
                        _print_donation_box()
                        code = process.wait()
                        return 0 if code == 0 else 1
                    if message not in ("initializing", "waiting_game"):
                        raise OSError(f"Сообщение клиента: {message}")
                    if message == "waiting_game":
                        with state_lock:
                            status_state["mode"] = "waiting_game"
            finally:
                process.close()
    finally:
        stop_anim.set()
        if anim_thread.is_alive():
            anim_thread.join(timeout=1)


def wait_until_ready(port, thread, timeout=10):
    deadline = time.monotonic() + timeout
    while thread.is_alive():
        try:
            check_server(port)
            return
        except (ConnectionRefusedError, ConnectionResetError, TimeoutError):
            if time.monotonic() >= deadline:
                break
            time.sleep(0.1)
    raise TimeoutError("Локальный сервер не стал готов к подключению")


def run_bundle(base=None, client_entry=None, port=800):
    base = Path(base or Path(__file__).resolve().parent).resolve()
    executable = base / "RobRun.exe"
    verify_local_image(executable.read_bytes())
    client_entry = client_entry or launch_robrun
    owned_server = None
    thread = None
    try:
        try:
            check_server(port)
        except (ConnectionRefusedError, ConnectionResetError, TimeoutError):
            try:
                owned_server = LocalServer(port, base / "data")
            except OSError:
                # Another copy may have started between the probe and the bind.
                check_server(port)
            else:
                thread = threading.Thread(
                    target=lambda: owned_server.serve_forever(poll_interval=0.1),
                    name="RobRunLocalAPI", daemon=True)
                thread.start()
                wait_until_ready(port, thread)
        return client_entry(executable)
    finally:
        if owned_server is not None:
            if thread is not None and thread.is_alive():
                owned_server.shutdown()
                thread.join(timeout=3)
            owned_server.server_close()


def run_server_only(port=800, data=None):
    data = Path(data or Path(__file__).resolve().parent / "data")
    try:
        server = LocalServer(port, data)
    except OSError as error:
        print(f"Не удалось открыть сервер: {error}", file=sys.stderr)
        return 1
    print(f"RobRun local API: http://127.0.0.1:{server.server_port}", flush=True)
    print(f"Данные: {server.state.directory}\nОстановка: Ctrl+C", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main(argv=None):
    configure_output()
    parser = argparse.ArgumentParser(description="Автономный локальный API RobRun")
    parser.add_argument("--port", type=int, default=800)
    parser.add_argument("--data", type=Path, default=None,
                        help="каталог данных (по умолчанию <папка скрипта>/data)")
    parser.add_argument("--server-only", action="store_true",
                        help="только локальный API, без запуска RobRun.exe")
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535:
        parser.error("Порт должен быть от 0 до 65535")
    if args.server_only:
        return run_server_only(args.port, args.data)
    try:
        return run_bundle(port=args.port)
    except (OSError, ValueError, TimeoutError) as error:
        print(f"Не удалось запустить комплект: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Запуск остановлен.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
