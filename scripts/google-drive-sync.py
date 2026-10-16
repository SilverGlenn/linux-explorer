#!/usr/bin/env python3
"""Private, selected-folder Google Drive synchronization worker.

Credentials are accepted only through bounded stdin.  Public commands exchange
bounded JSON over a private same-user Unix socket; child diagnostics are never
logged.  The streamed read-only mount and its configuration remain independent.
"""

import argparse
import contextlib
import datetime
import fcntl
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import re
import select
import selectors
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

sys.dont_write_bytecode = True


def load_mount_helper():
    directory = Path(__file__).resolve().parent
    for name in ("google-drive-rclone.py", "explorer-win7-google-drive"):
        path = directory / name
        if path.is_file():
            loader = importlib.machinery.SourceFileLoader("explorer_drive_mount", str(path))
            spec = importlib.util.spec_from_loader(loader.name, loader)
            module = importlib.util.module_from_spec(spec)
            loader.exec_module(module)
            return module
    raise RuntimeError("The Google Drive mount helper is unavailable.")


MOUNT = load_mount_helper()
Error = MOUNT.HelperError


class TransientError(Error):
    """Metadata/network failure before a write, safe to retry from the baseline."""


class MetadataTimeout(Error):
    """A whole metadata operation exhausted its shared monotonic budget."""


def metadata_timeout():
    return MetadataTimeout("Checking this Drive folder timed out. Try again, or select a smaller folder.")


def peer_disconnected(connection):
    """Inspect EOF without consuming request bytes or changing sync state."""
    try:
        if not select.select([connection], [], [], 0)[0]:
            return False
        return connection.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT) == b""
    except BlockingIOError:
        return False
    except OSError:
        return True

LIMIT = 64 * 1024
STATE_LIMIT = 2 * 1024 * 1024
INVENTORY_LIMIT = 16 * 1024 * 1024
MAX_ITEMS = 20000
MAX_PAIRS = 8
INTERVAL = 120
MIN_VERSION = (1, 71, 0)
REMOTE = MOUNT.REMOTE
FOLDER = "application/vnd.google-apps.folder"
ID_RE = re.compile(r"[A-Za-z0-9_-]{1,256}\Z")
PAIR_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z")
STATES = {"initializing", "syncing", "idle", "paused", "error", "needs-review"}
FILE_FIELDS = "id,name,mimeType,parents,driveId,trashed,size,md5Checksum,modifiedTime,capabilities(canEdit,canListChildren,canDownload,canAddChildren,canTrash)"


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def decode_json(data, limit=LIMIT):
    try:
        if len(data) > limit:
            raise ValueError()
        value = json.loads(data, object_pairs_hook=unique_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        if not isinstance(value, dict):
            raise ValueError()
        return value
    except (ValueError, UnicodeError, TypeError, RecursionError):
        raise Error("The sync request is invalid or exceeds the safety limit.") from None


def encoded(value):
    data = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    if len(data) > LIMIT:
        raise Error("The sync response exceeds the safety limit.")
    return data + b"\n"


def read_stdin(stream=None):
    if stream is not None:
        return decode_json(stream.read(LIMIT + 1))
    data = bytearray()
    deadline = time.monotonic() + 30
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([sys.stdin.fileno()], [], [], remaining)[0]:
            raise Error("The private sync request timed out.")
        chunk = os.read(sys.stdin.fileno(), min(4096, LIMIT + 1 - len(data)))
        if not chunk:
            return decode_json(data)
        data.extend(chunk)
        if len(data) > LIMIT:
            raise Error("The sync request exceeds the safety limit.")


def credentials(document):
    if set(document) != {"version", "client_id", "client_secret", "token", "config_password", "browse_config_password"}:
        raise Error("The private sync credentials are invalid.")
    base = {key: value for key, value in document.items() if key != "browse_config_password"}
    result = MOUNT.app_credentials(io.BytesIO(json.dumps(base).encode("utf-8")))
    browse = dict(base, token=None, config_password=document["browse_config_password"])
    MOUNT.app_credentials(io.BytesIO(json.dumps(browse).encode("utf-8")))
    return dict(result, browse_config_password=document["browse_config_password"])


def validate_id(value):
    if not isinstance(value, str) or not ID_RE.fullmatch(value) or value == "root":
        raise Error("Select an ordinary folder inside My Drive.")
    return value


def validate_name(value):
    if (not isinstance(value, str) or not value or len(value.encode("utf-8")) > 255
            or value in (".", "..") or "/" in value or "\\" in value
            or any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise Error("A Drive filename cannot be represented safely in the local folder.")
    return value


def overlap(left, right):
    return left == right or left.is_relative_to(right) or right.is_relative_to(left)


def public_pair(pair, account=None):
    result = {key: pair[key] for key in ("id", "remote_folder_id", "remote_name", "local_path", "state", "last_success", "error")}
    result["initialized"] = pair["initialized"]
    # This is UI eligibility: needs-review opens the explicit review flow;
    # the ordinary resume RPC still rejects that state.
    result["can_resume"] = (pair["initialized"] and pair["state"] in ("idle", "paused", "error", "needs-review")
                            and (account is None or pair["account"] == account))
    return result


def valid_relative(value):
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 4096:
        return False
    return all(component not in ("", ".", "..") and not any(ord(char) < 32 for char in component)
               for component in value.split("/"))


def binary_version(home):
    preferred = home / ".local/libexec/explorer-win7/rclone"
    binary = str(preferred) if preferred.exists() else shutil.which("rclone")
    if not binary:
        return None, "unavailable", False
    try:
        info = Path(binary).lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o022:
            raise ValueError()
        result = subprocess.run([binary, "version"], env=MOUNT.clean_environment(),
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=5, check=True)
        if len(result.stdout) > 8192:
            raise ValueError()
        first = result.stdout.decode("ascii").splitlines()[0]
        match = re.fullmatch(r"rclone v([0-9]+)\.([0-9]+)\.([0-9]+)", first)
        return binary, first.removeprefix("rclone "), bool(match and tuple(map(int, match.groups())) >= MIN_VERSION)
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError):
        return binary, "unverified", False


class Paths:
    def __init__(self, home=None, environment=None):
        self.helper = MOUNT.AppDriveHelper(home, environment)
        self.home = self.helper.home
        self.config = self.helper.config_dir / "app-sync-rclone.conf"
        self.directory = self.helper.config_dir / "sync"
        self.state_file = self.directory / "state.json"
        self.socket = self.directory / "worker.sock"
        self.lock_file = self.directory / "worker.lock"

    def prepare(self):
        self.helper.safe_directory(self.directory, create=True)
        if len(os.fsencode(self.socket)) >= 104:
            raise Error("The private sync directory path is too long for a local socket.")

    def read_state(self):
        if not self.state_file.exists() and not self.state_file.is_symlink():
            return {"version": 1, "pairs": []}
        with MOUNT.checked_file(self.state_file, private=True) as stream:
            state = decode_json(stream.read(STATE_LIMIT + 1), STATE_LIMIT)
        if set(state) != {"version", "pairs"} or type(state["version"]) is not int or state["version"] != 1 or not isinstance(state["pairs"], list) or len(state["pairs"]) > MAX_PAIRS:
            raise Error("The saved sync settings are invalid.")
        ids = set()
        for pair in state["pairs"]:
            if (not isinstance(pair, dict) or set(pair) not in (
                    {"id", "account", "remote_folder_id", "remote_name", "local_path", "local_device", "local_inode", "state", "last_success", "error", "initialized", "reviewed_hash"},
                    {"id", "account", "remote_folder_id", "remote_name", "local_path", "local_device", "local_inode", "state", "last_success", "error", "initialized", "reviewed_hash", "deletion_review"})
                    or not isinstance(pair["id"], str) or not PAIR_RE.fullmatch(pair["id"])
                    or pair["id"] in ids or not isinstance(pair["state"], str) or pair["state"] not in STATES
                    or not isinstance(pair["account"], str) or not re.fullmatch(r"[0-9a-f]{64}", pair["account"])
                    or type(pair["initialized"]) is not bool
                    or not isinstance(pair["reviewed_hash"], str) or not re.fullmatch(r"[0-9a-f]{64}", pair["reviewed_hash"])
                    or not isinstance(pair["local_path"], str)
                    or len(pair["local_path"].encode("utf-8")) > 1024
                    or type(pair["local_device"]) is not int or type(pair["local_inode"]) is not int
                    or not isinstance(pair["last_success"], str) or len(pair["last_success"]) > 40
                    or not isinstance(pair["error"], str) or len(pair["error"]) > 512):
                raise Error("The saved sync pair is invalid.")
            validate_id(pair["remote_folder_id"])
            validate_name(pair["remote_name"])
            approval = pair.get("deletion_review")
            if approval is not None and (not isinstance(approval, dict)
                    or set(approval) != {"fingerprint", "local_deleted", "remote_deleted", "expires"}
                    or not isinstance(approval["fingerprint"], str) or not re.fullmatch(r"[0-9a-f]{64}", approval["fingerprint"])
                    or type(approval["expires"]) is not int or approval["expires"] < 0
                    or any(not isinstance(approval[label], list) or len(approval[label]) > 10
                           or any(not valid_relative(name) for name in approval[label])
                           for label in ("local_deleted", "remote_deleted"))):
                raise Error("The saved deletion review is invalid.")
            pair.setdefault("deletion_review", None)
            ids.add(pair["id"])
        return state

    def save(self, state):
        self.prepare()
        self.atomic_json(self.state_file, state, STATE_LIMIT)

    def atomic_json(self, path, document, limit=INVENTORY_LIMIT):
        data = json.dumps(document, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        if len(data) > limit:
            raise Error("The sync inventory exceeds the safety limit.")
        self.helper.safe_directory(path.parent, create=True)
        fd, pending = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(pending, path)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(pending)

    @contextlib.contextmanager
    def lock(self, nonblocking=True):
        self.prepare()
        try:
            fd = os.open(self.lock_file, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        except OSError:
            raise Error("The private sync lock is inaccessible or unsafe.") from None
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
                raise Error("The private sync lock is unsafe.")
            os.fchmod(fd, 0o600)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0))
            except BlockingIOError:
                raise Error("The Google Drive sync worker is already running.") from None
            yield
        finally:
            os.close(fd)

    def local(self, value, pairs=(), empty=False, existing_pair=None):
        if not isinstance(value, str) or len(value.encode("utf-8")) > 1024:
            raise Error("Choose an empty local folder inside your home directory.")
        path = Path(value)
        if (not path.is_absolute() or path == self.home or ".." in path.parts
                or not path.is_relative_to(self.home)):
            raise Error("Choose an ordinary local folder inside your home directory.")
        forbidden = (self.helper.config_dir, self.helper.mount,
                     self.home / ".cache", self.home / ".local/libexec/explorer-win7")
        if any(overlap(path, item) for item in forbidden):
            raise Error("The local sync folder overlaps application data, cache, or the Drive mount.")
        self.helper.safe_directory(path)
        try:
            info = path.lstat()
        except OSError:
            raise Error("The local folder must already exist.") from None
        if not stat.S_ISDIR(info.st_mode):
            raise Error("The local sync folder is not an ordinary directory.")
        if existing_pair and (info.st_dev != existing_pair["local_device"] or info.st_ino != existing_pair["local_inode"]):
            raise Error("The local sync directory was replaced. Review this pair before continuing.")
        for pair in pairs:
            if pair is not existing_pair and overlap(path, Path(pair["local_path"])):
                raise Error("Local sync folders must not overlap one another.")
        try:
            with Path("/proc/self/mountinfo").open() as stream:
                mountinfo = stream.read(1024 * 1024 + 1)
            if len(mountinfo) > 1024 * 1024:
                raise Error("The mount table exceeds the safety limit.")
            for line in mountinfo.splitlines():
                fields = line.split()
                if len(fields) > 5:
                    mount = Path(MOUNT.decode_mount_field(fields[4]))
                    if mount == path or mount.is_relative_to(path):
                        raise Error("The local sync folder must not contain a mounted filesystem.")
            if empty and next(path.iterdir(), None) is not None:
                raise Error("Choose an empty local folder for the initial cloud download.")
        except OSError:
            raise Error("The local sync folder is inaccessible.") from None
        return path, info


class Rclone:
    def __init__(self, paths, secret, binary):
        self.paths, self.secret, self.binary = paths, secret, binary
        self.cancel = threading.Event()

    def run(self, arguments, *, config=None, password=None, timeout=1800, limit=INVENTORY_LIMIT, pass_fds=(), password_command=None, deadline=None):
        metadata_deadline = deadline
        if metadata_deadline is not None and time.monotonic() >= metadata_deadline:
            raise metadata_timeout()
        command = [self.binary, "--config", str(config or self.paths.config), "--ask-password=false",
                   "--password-command", MOUNT.rclone_command_string(password_command or [MOUNT.require_binary("cat")]),
                   "--log-level", "ERROR", "--stats", "0", "--contimeout", "15s", "--timeout", "60s",
                   "--transfers", "2", "--checkers", "2", "--buffer-size", "4M", "--drive-chunk-size", "8M",
                   "--tpslimit", "4", "--drive-use-trash=true"] + list(arguments)
        data = bytearray()
        process = subprocess.Popen(command, env=MOUNT.clean_environment(), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, close_fds=True, pass_fds=pass_fds)
        deadline = min(time.monotonic() + timeout, metadata_deadline) if metadata_deadline is not None else time.monotonic() + timeout
        selector = selectors.DefaultSelector()
        try:
            process.stdin.write((password or self.secret).encode("ascii") + b"\n")
            process.stdin.close()
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if self.cancel.is_set():
                    raise Error("Synchronization was paused or stopped.")
                if time.monotonic() >= deadline:
                    if metadata_deadline is not None:
                        raise metadata_timeout()
                    raise Error("Synchronization timed out. The existing baseline was retained.")
                for key, _mask in selector.select(0.2):
                    chunk = os.read(key.fileobj.fileno(), 16384)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        data.extend(chunk)
                        if len(data) > limit:
                            raise Error("The sync command output exceeds the safety limit.")
            if process.wait(timeout=5) != 0:
                raise Error("The Drive operation failed. Check connectivity and permissions; the baseline was retained.")
            return bytes(data)
        except (OSError, subprocess.SubprocessError):
            raise Error("The Drive operation could not complete. The existing baseline was retained.") from None
        finally:
            selector.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()

    def config_document(self, path, password, scope, client_id, client_secret, refresh=True, deadline=None):
        if not MOUNT.encrypted_config(path):
            raise Error("An encrypted application configuration is required.")
        with MOUNT.checked_file(path, private=True) as stream:
            if os.fstat(stream.fileno()).st_size > 256 * 1024:
                raise Error("The encrypted application configuration exceeds the safety limit.")
        data = self.run(["config", "dump"], config=path, password=password, timeout=15, limit=256 * 1024, deadline=deadline)
        try:
            document = decode_json(data, 256 * 1024)
            remote = document[REMOTE[:-1]]
            if (set(document) != {REMOTE[:-1]} or not isinstance(remote, dict)
                    or set(remote) != {"type", "scope", "client_id", "client_secret", "token"}
                    or remote["type"] != "drive" or remote["scope"] != scope
                    or remote["client_id"] != client_id or remote["client_secret"] != client_secret):
                raise ValueError()
            envelope = {"version": 1, "client_id": client_id, "client_secret": client_secret,
                        "config_password": password, "token": json.loads(remote["token"])}
            token = MOUNT.app_credentials(io.BytesIO(json.dumps(envelope).encode("utf-8")))["token"]
        except (KeyError, TypeError, ValueError, Error):
            raise Error("The encrypted configuration does not match this application and permission.") from None
        if refresh:
            # Establish the exact backend/client/scope before any network call.
            try:
                self.run(["about", REMOTE, "--json"], config=path, password=password, timeout=45, limit=LIMIT, deadline=deadline)
            except MetadataTimeout:
                raise
            except Error:
                raise TransientError("The Drive session could not refresh. Check connectivity or authorization; synchronization will retry automatically.") from None
            return self.config_document(path, password, scope, client_id, client_secret, refresh=False, deadline=deadline)
        return token

    def install(self, credential):
        if credential["token"] is None:
            if not self.paths.config.exists():
                raise Error("Grant two-way Drive permission before starting synchronization.")
            return
        helper = MOUNT.AppDriveHelper(self.paths.home, {"XDG_CONFIG_HOME": str(self.paths.helper.config_dir.parents[1])})
        helper.config = self.paths.config
        helper.safe_directory(helper.config_dir, create=True)
        if self.paths.config.exists() or self.paths.config.is_symlink():
            if not MOUNT.encrypted_config(self.paths.config):
                raise Error("The existing sync configuration is unsafe.")
        token = json.dumps(credential["token"], ensure_ascii=True, separators=(",", ":"))
        content = ("[explorer-google-drive]\ntype = drive\nscope = drive\nclient_id = "
                   + credential["client_id"] + "\nclient_secret = " + credential["client_secret"]
                   + "\ntoken = " + token + "\n").encode("ascii")
        if not hasattr(os, "memfd_create"):
            raise Error("This system cannot securely encrypt application credentials.")
        fd = os.memfd_create("explorer-drive-sync-config", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        try:
            os.fchmod(fd, 0o600)
            os.write(fd, content)
            del content, token
            fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                        fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
            with tempfile.TemporaryDirectory(prefix=".sync-setup-", dir=helper.config_dir) as temporary:
                pending = Path(temporary) / "rclone.conf"
                pending.symlink_to(f"/proc/self/fd/{fd}")
                # The command loads the sealed plaintext memfd before asking
                # its password command for the new key.  Modern rclone follows
                # symlinks when saving, so remove this controlled link at that
                # exact handoff, before the encrypted writer creates its file.
                # No plaintext configuration or encryption key reaches disk.
                key_command = [sys.executable, "-c",
                               "import os,sys; "
                               "(os.environ.get('RCLONE_PASSWORD_CHANGE') == '1' and os.path.islink(sys.argv[1])) or sys.exit(1); "
                               "os.unlink(sys.argv[1]); "
                               "sys.stdout.buffer.write(sys.stdin.buffer.read(129))",
                               str(pending)]
                self.run(["config", "encryption", "set"], config=pending,
                         password=credential["config_password"], timeout=30, limit=LIMIT, pass_fds=(fd,), password_command=key_command)
                if not MOUNT.encrypted_config(pending):
                    raise Error("rclone did not encrypt the sync configuration.")
                os.replace(pending, self.paths.config)
        finally:
            os.close(fd)


class DriveAPI:
    def __init__(self, token):
        self.token = token
        self.refresh = None
        self.token_lock = threading.Lock()
        self.cancel = threading.Event()
        self.context = threading.local()

    @property
    def deadline(self):
        return getattr(self.context, "deadline", None)

    @contextlib.contextmanager
    def budget(self, seconds, peer=None):
        previous_deadline = self.deadline
        previous_peer = getattr(self.context, "peer", None)
        proposed = time.monotonic() + seconds
        self.context.deadline = min(previous_deadline, proposed) if previous_deadline is not None else proposed
        self.context.peer = peer if peer is not None else previous_peer
        try:
            self.check_cancel()
            yield
        finally:
            self.context.deadline, self.context.peer = previous_deadline, previous_peer

    def remaining(self):
        self.check_cancel()
        return max(0.001, self.deadline - time.monotonic()) if self.deadline is not None else None

    def http_timeout(self):
        remaining = self.remaining()
        return min(15, remaining) if remaining is not None else 15

    def check_cancel(self):
        if self.cancel.is_set():
            raise Error("Synchronization was paused or stopped.")
        if self.deadline is not None and time.monotonic() >= self.deadline:
            raise metadata_timeout()
        peer = getattr(self.context, "peer", None)
        if peer is not None and peer():
            raise Error("The folder check was cancelled when its client disconnected. Try checking the folder again.")

    def response_json(self, response):
        data = bytearray()
        limit = 2 * 1024 * 1024
        while True:
            self.check_cancel()
            # urllib's HTTPResponse exposes the underlying socket through its
            # buffered reader. Refresh its timeout as the total budget shrinks.
            raw = getattr(getattr(response, "fp", None), "raw", None)
            transport = getattr(raw, "_sock", None)
            if transport is not None:
                transport.settimeout(self.http_timeout())
            read = getattr(response, "read1", response.read)
            chunk = read(min(16384, limit + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > limit:
                raise Error("Drive returned metadata exceeding the safety limit. Select a smaller folder.")
        self.check_cancel()
        return decode_json(data, limit)

    def get(self, endpoint, parameters):
        self.check_cancel()
        remaining = self.remaining()
        acquired = self.token_lock.acquire(timeout=remaining) if remaining is not None else self.token_lock.acquire()
        if not acquired:
            raise metadata_timeout()
        try:
            self.check_cancel()
            try:
                expires = datetime.datetime.fromisoformat(self.token["expiry"].replace("Z", "+00:00"))
                near_expiry = expires.timestamp() <= time.time() + 90
            except (KeyError, TypeError, ValueError):
                near_expiry = True
            if near_expiry and self.refresh:
                self.token = self.refresh(deadline=self.deadline)
            self.check_cancel()
        finally:
            self.token_lock.release()
        url = "https://www.googleapis.com/drive/v3/" + endpoint + "?" + urllib.parse.urlencode(parameters)
        request = urllib.request.Request(url, headers={"Authorization": "Bearer " + self.token["access_token"]})
        # Do not follow redirects carrying the authorization header.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                return None
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        for attempt in range(3):
            self.check_cancel()
            try:
                with opener.open(request, timeout=self.http_timeout()) as response:
                    return self.response_json(response)
            except urllib.error.HTTPError as error:
                code = error.code
                error.close()
                if code not in (429, 500, 502, 503, 504):
                    raise Error("Drive metadata is unavailable. Check permission and connectivity.") from None
                if attempt == 2:
                    raise TransientError("Drive is temporarily unavailable or rate limited. Synchronization will retry automatically.") from None
            except (OSError, urllib.error.URLError):
                self.check_cancel()
                if attempt == 2:
                    raise TransientError("Drive is offline or unreachable. Synchronization will retry automatically.") from None
            remaining = self.remaining()
            sleep = min(1 << attempt, remaining) if remaining is not None else 1 << attempt
            if self.cancel.wait(sleep):
                self.check_cancel()
            self.check_cancel()
        raise Error("Drive metadata is unavailable.")

    def account(self):
        result = self.get("about", {"fields": "user(permissionId)"})
        value = result.get("user", {}).get("permissionId")
        if not isinstance(value, str) or not ID_RE.fullmatch(value):
            raise Error("Drive did not provide a verified account identity.")
        return hashlib.sha256(value.encode("ascii")).hexdigest()

    def metadata(self, folder_id):
        return self.get("files/" + urllib.parse.quote(folder_id, safe=""), {"fields": FILE_FIELDS, "supportsAllDrives": "true"})

    def folder(self, folder_id, allow_root=False):
        if folder_id != "root":
            validate_id(folder_id)
        elif not allow_root:
            raise Error("Select a folder inside My Drive, rather than its root.")
        root = self.metadata("root")
        item = self.metadata(folder_id)
        current = item
        seen = set()
        for _ in range(100):
            self.check_cancel()
            if (current.get("mimeType") != FOLDER or current.get("driveId") or current.get("trashed")
                    or current.get("id") in seen):
                raise Error("Only ordinary folders inside My Drive are supported.")
            seen.add(current.get("id"))
            if current.get("id") == root.get("id"):
                if not allow_root and item.get("id") == root.get("id"):
                    raise Error("Select a folder inside My Drive, rather than its root.")
                validate_name(item.get("name"))
                capabilities = item.get("capabilities", {})
                if not capabilities.get("canListChildren") or (not allow_root and not capabilities.get("canAddChildren")):
                    raise Error("The selected folder does not allow synchronization.")
                return item, seen
            parents = current.get("parents")
            if not isinstance(parents, list) or len(parents) != 1:
                raise Error("The folder cannot be verified beneath My Drive.")
            validate_id(parents[0])
            current = self.metadata(parents[0])
        raise Error("The folder ancestry exceeds the safety limit.")

    def children(self, folder_id):
        validate_id(folder_id)
        result, page = [], None
        while True:
            self.check_cancel()
            parameters = {"q": "'" + folder_id + "' in parents and trashed=false", "fields": "nextPageToken,incompleteSearch,files(" + FILE_FIELDS + ")",
                          "pageSize": "1000", "spaces": "drive", "corpora": "user", "supportsAllDrives": "true"}
            if page:
                parameters["pageToken"] = page
            document = self.get("files", parameters)
            if document.get("incompleteSearch") or not isinstance(document.get("files"), list):
                raise Error("Drive returned an incomplete folder listing.")
            result.extend(document["files"])
            if len(result) > MAX_ITEMS:
                raise Error("The selected folder exceeds the 20,000 item safety limit.")
            page = document.get("nextPageToken")
            if not page:
                return result
            if not isinstance(page, str) or len(page) > 8192:
                raise Error("Drive returned an invalid listing token.")

    def inventory(self, folder_id, backup_name=None, sentinel=None):
        # Include ancestry and every page in one budget, rather than starting
        # a fresh timer after the potentially slow parent-folder validation.
        with self.budget(120):
            return self._inventory(folder_id, backup_name, sentinel)

    def _inventory(self, folder_id, backup_name=None, sentinel=None):
        root, ancestry = self.folder(folder_id)
        queue, files, count, total, seen = [(folder_id, "")], {}, 0, 0, set()
        while queue:
            self.check_cancel()
            if len(seen) > 2000:
                raise Error("The folder tree is too large to inspect safely. Select a smaller folder.")
            parent, prefix = queue.pop()
            if parent in seen:
                raise Error("The Drive folder tree contains a repeated folder.")
            seen.add(parent)
            names = set()
            for item in self.children(parent):
                self.check_cancel()
                count += 1
                if count > MAX_ITEMS:
                    raise Error("The selected folder exceeds the 20,000 item safety limit.")
                name = validate_name(item.get("name"))
                if name in names:
                    raise Error("Duplicate Drive names must be resolved before synchronization.")
                names.add(name)
                validate_id(item.get("id"))
                if item.get("driveId") or item.get("trashed") or item.get("parents") != [parent]:
                    raise Error("The Drive folder listing contains an unsupported item.")
                mime = item.get("mimeType", "")
                if not prefix and backup_name and name == backup_name:
                    if mime != FOLDER:
                        raise Error("The remote backup location was replaced.")
                    continue
                if name.startswith(".explorer-sync-") and not (not prefix and name == sentinel):
                    raise Error("The selected folder contains reserved sync metadata names.")
                relative = prefix + name
                if mime == FOLDER:
                    if not item.get("capabilities", {}).get("canListChildren"):
                        raise Error("A Drive subfolder cannot be fully listed.")
                    queue.append((item["id"], relative + "/"))
                else:
                    if not isinstance(mime, str) or mime.startswith("application/vnd.google-apps."):
                        raise Error("Google-native documents and shortcuts are not supported in two-way folders yet.")
                    if not item.get("capabilities", {}).get("canDownload") or not item.get("capabilities", {}).get("canEdit"):
                        raise Error("Every synchronized file must allow downloads and edits.")
                    try:
                        size = int(item["size"])
                        if size < 0 or size > (1 << 63):
                            raise ValueError()
                    except (KeyError, TypeError, ValueError):
                        raise Error("Drive returned an invalid file size.") from None
                    digest = item.get("md5Checksum")
                    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{32}", digest):
                        raise Error("A Drive file has no usable content checksum.")
                    files[relative] = {"id": item["id"], "size": size, "hash": digest}
                    total += size
        fingerprint = hashlib.sha256(json.dumps({"folder": root["id"], "name": root["name"], "files": files,
                                                 "folders": sorted(seen), "items": count, "bytes": total},
                                                sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
        return {"folder": root, "ancestry": ancestry, "files": files, "file_count": len(files), "bytes": total, "fingerprint": fingerprint}


def local_inventory(path, sentinel=None, hash_files=False, cancel=None, deadline=None, check=None):
    files, count = {}, 0
    root_device = path.stat().st_dev
    queue = [(path, "")]
    while queue:
        directory, prefix = queue.pop()
        try:
            with os.scandir(directory) as iterator:
                entries = [entry for _index, entry in zip(range(MAX_ITEMS - count + 1), iterator)]
        except OSError:
            raise Error("The local folder cannot be fully read.") from None
        for entry in entries:
            if check is not None:
                check()
            if deadline is not None and time.monotonic() >= deadline:
                raise metadata_timeout()
            if cancel is not None and cancel.is_set():
                raise Error("Synchronization was paused or stopped.")
            count += 1
            if count > MAX_ITEMS:
                raise Error("The local folder exceeds the 20,000 item safety limit.")
            name = validate_name(entry.name)
            info = entry.stat(follow_symlinks=False)
            if name.startswith(".explorer-sync-") and not (not prefix and name == sentinel):
                raise Error("The local folder contains reserved sync metadata names.")
            if (info.st_uid != os.getuid() or info.st_dev != root_device or info.st_mode & 0o002
                    or (info.st_mode & 0o020 and not MOUNT.private_primary_group(info.st_gid))):
                raise Error("A local sync entry has unsafe ownership, permissions, or filesystem.")
            if stat.S_ISDIR(info.st_mode):
                queue.append((Path(entry.path), prefix + name + "/"))
            elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
                if name.startswith(".explorer-sync-") and not (not prefix and name == sentinel):
                    raise Error("The local folder contains reserved sync metadata names.")
                with MOUNT.checked_file(Path(entry.path)) as stream:
                    before = os.fstat(stream.fileno())
                    digest = None
                    if hash_files:
                        digest = hashlib.md5(usedforsecurity=False)
                        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                            if check is not None:
                                check()
                            if deadline is not None and time.monotonic() >= deadline:
                                raise metadata_timeout()
                            if cancel is not None and cancel.is_set():
                                raise Error("Synchronization was paused or stopped.")
                            digest.update(chunk)
                    after = os.fstat(stream.fileno())
                if (before.st_mtime_ns, before.st_size, before.st_ino) != (after.st_mtime_ns, after.st_size, after.st_ino):
                    raise Error("A local file is still changing. Retry after it has been saved.")
                files[prefix + name] = {"size": after.st_size, "hash": digest.hexdigest() if digest else ""}
            else:
                raise Error("Symbolic links, hard links and special files are not supported in sync folders.")
    return files


class Worker:
    def __init__(self, paths, engine, api, account, version):
        self.paths, self.engine, self.api, self.account, self.version = paths, engine, api, account, version
        self.state = paths.read_state()
        self.mutex = threading.RLock()
        self.wakeup, self.stopping = threading.Event(), threading.Event()
        self.next_run, self.previews = {}, {}
        self.reviews = {}
        self.active = None
        self.scheduler = None
        for pair in self.state["pairs"]:
            if pair["account"] != account:
                pair["state"], pair["error"] = "needs-review", "This pair belongs to a different Drive account."
            elif pair["state"] in ("initializing", "syncing"):
                pair["state"], pair["error"] = "needs-review", "Synchronization was interrupted. Review the existing files before retrying."
        paths.save(self.state)

    def response(self, ok=True, message="", **extra):
        with self.mutex:
            public = [public_pair(pair, self.account) for pair in self.state["pairs"]]
        result = {"version": 1, "ok": ok, "message": message, "worker_running": not self.stopping.is_set() or self.active is not None,
                  "rclone_version": self.version, "available": True, "sync_configured": True, "pairs": public, **extra}
        encoded(result)
        return result

    def pair(self, pair_id):
        if not isinstance(pair_id, str) or not PAIR_RE.fullmatch(pair_id):
            raise Error("The sync pair ID is invalid.")
        for pair in self.state["pairs"]:
            if pair["id"] == pair_id:
                return pair
        raise Error("The selected sync pair no longer exists.")

    def check_remote_overlap(self, inventory, except_id=None):
        for pair in self.state["pairs"]:
            if pair["id"] == except_id:
                continue
            if pair["remote_folder_id"] in inventory["ancestry"]:
                raise Error("Remote sync folders must not overlap one another.")
            _other, ancestors = self.api.folder(pair["remote_folder_id"])
            if inventory["folder"]["id"] in ancestors:
                raise Error("Remote sync folders must not overlap one another.")

    def trusted_manifest(self, pair):
        manifest_path = self.paths.directory / "pairs" / pair["id"] / "manifest.json"
        with MOUNT.checked_file(manifest_path, private=True) as stream:
            data = stream.read(INVENTORY_LIMIT + 1)
        previous = decode_json(data, INVENTORY_LIMIT)
        if (set(previous) != {"local", "remote"}
                or any(not isinstance(previous[label], dict)
                       or any(not valid_relative(name) or not isinstance(value, dict) for name, value in previous[label].items())
                       for label in ("local", "remote"))):
            raise Error("The trusted synchronization inventory is unavailable.")
        return previous, hashlib.sha256(data).hexdigest()

    def deletion_snapshot(self, pair, path=None, inventory=None, local=None):
        if not pair["initialized"] or pair["account"] != self.account:
            raise Error("Only an initialized pair belonging to this Drive account can be reviewed.")
        path = path or self.paths.local(pair["local_path"], self.state["pairs"], existing_pair=pair)[0]
        sentinel = ".explorer-sync-access-" + pair["id"]
        backup = ".explorer-sync-backups-" + pair["id"]
        inventory = inventory or self.api.inventory(pair["remote_folder_id"], backup, sentinel)
        self.check_remote_overlap(inventory, pair["id"])
        local = local if local is not None else local_inventory(path, sentinel=sentinel, hash_files=True, cancel=self.engine.cancel,
                                                               deadline=self.api.deadline, check=self.api.check_cancel)
        previous, trusted_hash = self.trusted_manifest(pair)
        if sentinel not in local or sentinel not in inventory["files"]:
            raise Error("The access sentinel is missing. No deletion can be approved.")
        deleted = {"local_deleted": sorted(set(previous["local"]) - set(local)),
                   "remote_deleted": sorted(set(previous["remote"]) - set(inventory["files"]))}
        if len(set(deleted["local_deleted"]) | set(deleted["remote_deleted"])) > 10:
            raise Error("More than ten files were removed or renamed. Restore files or recreate the pair after reviewing both copies; this version cannot approve a larger deletion.")
        fingerprint = hashlib.sha256(json.dumps({"account": self.account, "folder_id": pair["remote_folder_id"],
                                                "local_device": pair["local_device"], "local_inode": pair["local_inode"],
                                                "manifest": trusted_hash, "local": local,
                                                "remote": inventory["fingerprint"], **deleted},
                                               sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
        return {"fingerprint": fingerprint, **deleted}, previous, local, inventory

    def request(self, request):
        if request.get("action") in ("list-folders", "preview", "review", "resume-reviewed"):
            with self.api.budget(120):
                return self._request(request)
        return self._request(request)

    def _request(self, request):
        if request.get("version") != 1 or type(request.get("version")) is not int:
            raise Error("The sync protocol version is invalid.")
        action = request.get("action")
        fields = {"status": set(), "stop": set(), "list-folders": {"parent_id"},
                  "preview": {"remote_folder_id", "local_path"}, "add": {"remote_folder_id", "local_path"},
                  "pause": {"pair_id"}, "resume": {"pair_id"}, "sync-now": {"pair_id"}, "remove": {"pair_id"},
                  "review": {"pair_id"}, "resume-reviewed": {"pair_id", "review_token"}}
        if action not in fields or set(request) - {"version", "action"} != fields[action]:
            if not (action == "list-folders" and set(request) == {"version", "action"}):
                raise Error("The sync action or its fields are invalid.")
        if action == "status":
            return self.response()
        if action == "stop":
            self.stopping.set()
            self.engine.cancel.set()
            self.wakeup.set()
            if self.scheduler:
                self.scheduler.join(timeout=90)
                if self.scheduler.is_alive():
                    return self.response(False, "Synchronization is still stopping. Wait before disconnecting or revoking access.")
            return self.response(message="Google Drive synchronization is stopping.")
        if action == "list-folders":
            parent, _ancestry = self.api.folder(request.get("parent_id", "root"), allow_root=True)
            folders, names = [], set()
            for item in self.api.children(parent["id"]):
                name = validate_name(item.get("name"))
                if name in names:
                    raise Error("This Drive folder contains duplicate names. Resolve them before selecting a sync folder.")
                names.add(name)
                if item.get("mimeType") == FOLDER and not item.get("driveId") and not name.startswith(".explorer-sync-"):
                    folders.append({"id": validate_id(item.get("id")), "name": name})
            return self.response(folders=sorted(folders, key=lambda item: item["name"].casefold()), parent_id=parent["id"], parent_name=parent["name"])
        if action in ("preview", "add"):
            folder_id = validate_id(request.get("remote_folder_id"))
            with self.mutex:
                path, info = self.paths.local(request.get("local_path"), self.state["pairs"], empty=True)
            key = (folder_id, str(path))
            if action == "preview":
                inventory = self.api.inventory(folder_id)
                with self.mutex:
                    self.check_remote_overlap(inventory)
                    self.previews[key] = (time.monotonic(), info.st_dev, info.st_ino, inventory["folder"]["name"], inventory["fingerprint"])
                free = shutil.disk_usage(path).free
                if inventory["bytes"] + 64 * 1024 * 1024 > free:
                    raise Error("The local disk has insufficient space for this folder and safety margin.")
                return self.response(preview={"folder_id": folder_id, "name": inventory["folder"]["name"], "file_count": inventory["file_count"], "bytes": inventory["bytes"], "unsupported": []})
            with self.mutex:
                preview = self.previews.get(key)
                if not preview or time.monotonic() - preview[0] > 300 or preview[1:3] != (info.st_dev, info.st_ino):
                    raise Error("Preview this folder and its empty local destination before starting sync.")
                if len(self.state["pairs"]) >= MAX_PAIRS:
                    raise Error("At most eight selected-folder sync pairs are supported.")
                pair = {"id": str(uuid.uuid4()), "account": self.account, "remote_folder_id": folder_id,
                        "remote_name": preview[3], "local_path": str(path), "local_device": info.st_dev,
                        "local_inode": info.st_ino, "state": "initializing", "last_success": "", "error": "", "initialized": False,
                        "reviewed_hash": preview[4], "deletion_review": None}
                self.state["pairs"].append(pair)
                self.paths.save(self.state)
                self.previews.pop(key)
                self.next_run[pair["id"]] = 0
                self.wakeup.set()
            return self.response(message="The initial cloud download has been queued.")
        with self.mutex:
            pair = self.pair(request.get("pair_id"))
            if action in ("review", "resume-reviewed"):
                if self.active == pair["id"]:
                    raise Error("Wait for this folder's current synchronization to finish before reviewing it.")
                snapshot, _previous, _local, _inventory = self.deletion_snapshot(pair)
                if action == "review":
                    self.reviews = {token: item for token, item in self.reviews.items()
                                    if item["pair_id"] != pair["id"] and item["expires"] >= time.time()}
                    token = os.urandom(32).hex()
                    self.reviews[token] = {"pair_id": pair["id"], "expires": int(time.time()) + 300, **snapshot}
                    return self.response(review={"pair_id": pair["id"], "token": token,
                                                 "local_deleted": snapshot["local_deleted"], "remote_deleted": snapshot["remote_deleted"],
                                                 "message": "Approve only these removals for the next sync. Retained backups, access checks and conflict preservation remain enabled."})
                token = request.get("review_token")
                if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token):
                    raise Error("The deletion review token is invalid.")
                reviewed = self.reviews.pop(token, None)
                if (not reviewed or reviewed["pair_id"] != pair["id"] or reviewed["expires"] < time.time()
                        or reviewed["fingerprint"] != snapshot["fingerprint"]):
                    raise Error("The folder changed or its review expired. Review the current changes again.")
                pair["deletion_review"] = {**snapshot, "expires": reviewed["expires"]}
                pair["state"], pair["error"] = "idle", ""
                self.next_run[pair["id"]] = 0
                self.paths.save(self.state)
                self.wakeup.set()
                return self.response(message="The reviewed changes were approved for one synchronization.")
            if action == "pause":
                pair["state"] = "paused"
                if self.active == pair["id"]:
                    self.engine.cancel.set()
            elif action == "remove":
                if self.active == pair["id"]:
                    raise Error("Pause this pair and wait for synchronization to stop before removing it.")
                self.state["pairs"].remove(pair)
            else:
                if pair["account"] != self.account:
                    raise Error("This sync pair belongs to another Drive account.")
                if not pair["initialized"]:
                    raise Error("An interrupted initial download must be reviewed and removed before creating a new pair.")
                if pair["state"] == "needs-review":
                    raise Error("Review this pair's current changes before resuming synchronization.")
                if self.active == pair["id"]:
                    return self.response(message="This folder is already synchronizing.")
                pair["state"], pair["error"] = "idle", ""
                self.next_run[pair["id"]] = 0
                self.wakeup.set()
            self.paths.save(self.state)
        return self.response(message="The sync pair was updated. Local and cloud files were retained.")

    def run_pair(self, pair):
        path, _info = self.paths.local(pair["local_path"], self.state["pairs"], empty=not pair["initialized"], existing_pair=pair)
        pair_dir = self.paths.directory / "pairs" / pair["id"]
        self.paths.helper.safe_directory(pair_dir, create=True)
        sentinel = ".explorer-sync-access-" + pair["id"]
        backup = ".explorer-sync-backups-" + pair["id"]
        inventory = self.api.inventory(pair["remote_folder_id"], backup if pair["initialized"] else None,
                                       sentinel if pair["initialized"] else None)
        if not pair["initialized"] and inventory["fingerprint"] != pair["reviewed_hash"]:
            raise Error("The cloud folder changed after its preview. No download was started; remove this pair and preview it again.")
        self.check_remote_overlap(inventory, pair["id"])
        root_flags = ["--drive-root-folder-id", pair["remote_folder_id"], "--drive-skip-gdocs", "--drive-skip-shortcuts",
                      "--exclude", "/" + backup + "/**", "--exclude", "/" + backup + "/", "--disable", "ListR"]
        manifest_path = pair_dir / "manifest.json"
        max_delete_percent = 10
        approved = False
        if pair["initialized"]:
            previous, _trusted_hash = self.trusted_manifest(pair)
            approval = pair.get("deletion_review")
            local = local_inventory(path, sentinel=sentinel, hash_files=bool(approval), cancel=self.engine.cancel)
            if sentinel not in local or sentinel not in inventory["files"]:
                raise Error("The access sentinel is missing. No files were synchronized.")
            deleted_sets = {"local_deleted": sorted(set(previous["local"]) - set(local)),
                            "remote_deleted": sorted(set(previous["remote"]) - set(inventory["files"]))}
            if len(set(deleted_sets["local_deleted"]) | set(deleted_sets["remote_deleted"])) > 10:
                raise Error("More than ten files were removed or renamed. Restore files or recreate the pair after reviewing both copies; this version cannot approve a larger deletion.")
            if approval:
                snapshot, _previous, _local, _inventory = self.deletion_snapshot(pair, path, inventory, local)
                if (approval["expires"] < time.time() or approval["expires"] > time.time() + 301
                        or snapshot["fingerprint"] != approval["fingerprint"]
                        or any(snapshot[label] != approval[label] for label in deleted_sets)):
                    with self.mutex:
                        pair["deletion_review"] = None
                        self.paths.save(self.state)
                    raise Error("The approved folder state changed or the deletion review expired. Review its current changes again.")
                approved = True
                percentages = [math.ceil(100 * len(deleted_sets[label + "_deleted"]) / max(1, len(previous[label])))
                               for label in ("local", "remote")]
                max_delete_percent = max(percentages) if any(percentages) else 10
            for label, current in (("local", local), ("remote", inventory["files"])):
                deleted = deleted_sets[label + "_deleted"]
                if not approved and deleted and len(deleted) * 100 > max(1, len(previous[label])) * 10:
                    raise Error("More than ten percent of files were removed or renamed. Review this pair's exact deletions before resuming.")
            incoming = sum(item["size"] for name, item in inventory["files"].items()
                           if name not in local or previous["remote"].get(name, {}).get("hash") != item["hash"])
            # Backups retain overwritten bytes, so reserve complete incoming
            # versions rather than only their growth relative to old files.
            margin = 64 * 1024 * 1024
            if incoming + margin > shutil.disk_usage(path).free or margin > shutil.disk_usage(pair_dir).free:
                raise Error("There is insufficient free disk space for incoming files and retained backups. Free space before resuming this pair.")
        else:
            if inventory["bytes"] + 64 * 1024 * 1024 > shutil.disk_usage(path).free:
                raise Error("The local disk has insufficient space for the initial cloud download.")
            self.engine.run(["copy", REMOTE, str(path), "--ignore-existing", "--create-empty-src-dirs"] + root_flags)
            local = local_inventory(path, hash_files=True, cancel=self.engine.cancel)
            if set(local) != set(inventory["files"]) or any(local[name]["hash"] != value["hash"] for name, value in inventory["files"].items()):
                raise Error("The initial download changed during preparation. Existing files were retained; review and recreate the pair.")
            sentinel_path = path / sentinel
            fd = os.open(sentinel_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write((pair["id"] + "\n" + self.account + "\n").encode("ascii"))
            # Single-file copyto rejects directory filters; the sentinel is an
            # exact app-owned name under the already validated folder ID.
            self.engine.run(["copyto", str(sentinel_path), REMOTE + sentinel, "--immutable",
                             "--drive-root-folder-id", pair["remote_folder_id"]])
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
        backups = pair_dir / "backups" / stamp
        self.paths.helper.safe_directory(backups, create=True)
        workdir = pair_dir / "bisync"
        self.paths.helper.safe_directory(workdir, create=True)
        arguments = ["bisync", str(path), REMOTE, "--workdir", str(workdir), "--check-access", "--check-filename", sentinel,
                     "--max-delete", str(max_delete_percent), "--conflict-resolve", "none", "--conflict-loser", "num",
                     "--conflict-suffix", "explorer-conflict",
                     "--backup-dir1", str(backups), "--backup-dir2", REMOTE + backup + "/" + stamp,
                     "--create-empty-src-dirs", "--resilient", "--recover", "--retries", "3", "--retries-sleep", "10s"] + root_flags
        if not pair["initialized"]:
            arguments += ["--resync-mode", "path2"]
        if approved:
            # Persist consumption before any write. A failure or interruption
            # cannot replay this authorization on a later synchronization.
            with self.mutex:
                pair["deletion_review"] = None
                self.paths.save(self.state)
        self.engine.run(arguments)
        final_remote = self.api.inventory(pair["remote_folder_id"], backup, sentinel)
        final_local = local_inventory(path, sentinel=sentinel, cancel=self.engine.cancel)
        self.paths.atomic_json(manifest_path, {"local": final_local, "remote": final_remote["files"]})
        new_conflicts = pair["initialized"] and any(
            name not in local and re.search(r"\.explorer-conflict[0-9]+(?:\.|$)", name) for name in final_local)
        pair["initialized"] = True
        return bool(new_conflicts)

    def schedule(self):
        while not self.stopping.is_set():
            selected = None
            with self.mutex:
                for pair in self.state["pairs"]:
                    if pair["state"] in ("idle", "initializing", "error") and self.next_run.get(pair["id"], 0) <= time.monotonic():
                        selected = pair
                        self.active = pair["id"]
                        if pair["initialized"]:
                            pair["state"] = "syncing"
                        self.engine.cancel.clear()
                        self.paths.save(self.state)
                        break
            if selected is None:
                self.wakeup.wait(1)
                self.wakeup.clear()
                continue
            try:
                conflicts = self.run_pair(selected)
                with self.mutex:
                    if selected["state"] != "paused":
                        selected["state"] = "needs-review" if conflicts else "idle"
                        selected["error"] = ("Both edited versions were saved with explorer-conflict suffixes. Resolve the conflict copies and resume this pair."
                                             if conflicts else "")
                    selected["last_success"] = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
            except TransientError as error:
                with self.mutex:
                    if selected["state"] != "paused":
                        selected["state"] = "error" if selected["initialized"] else "needs-review"
                    selected["error"] = str(error)
            except Error as error:
                with self.mutex:
                    if selected["state"] != "paused":
                        selected["state"] = "needs-review"
                    selected["error"] = str(error)
            except Exception:
                with self.mutex:
                    selected["state"], selected["error"] = "needs-review", "Synchronization stopped unexpectedly. Existing files and baseline were retained."
            finally:
                with self.mutex:
                    self.next_run[selected["id"]] = time.monotonic() + INTERVAL
                    self.active = None
                    if not self.stopping.is_set():
                        self.engine.cancel.clear()
                    self.paths.save(self.state)

    def serve(self, ready=None):
        self.paths.prepare()
        if self.paths.socket.exists() or self.paths.socket.is_symlink():
            info = self.paths.socket.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise Error("The private sync socket location is unsafe.")
            self.paths.socket.unlink()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(str(self.paths.socket))
            os.chmod(self.paths.socket, 0o600)
            server.listen(4)
            server.settimeout(1)
            self.scheduler = threading.Thread(target=self.schedule, name="drive-sync", daemon=True)
            self.scheduler.start()
            if ready:
                ready(self.response(message="Google Drive synchronization is running."))
            while not self.stopping.is_set():
                try:
                    connection, _address = server.accept()
                except socket.timeout:
                    continue
                with connection:
                    try:
                        _pid, uid, _gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
                        if uid != os.getuid():
                            continue
                        request = receive_socket(connection)
                        if peer_disconnected(connection):
                            continue
                        if request.get("action") in ("list-folders", "preview", "review", "resume-reviewed"):
                            # Only this read-only RPC is abandoned when its
                            # caller leaves. The background sync cancel event
                            # and any active synchronization remain unchanged.
                            with self.api.budget(120, peer=lambda: peer_disconnected(connection)):
                                response = self.request(request)
                        else:
                            response = self.request(request)
                    except Error as error:
                        response = self.response(False, str(error))
                    except Exception:
                        response = self.response(False, "The sync request could not be completed safely.")
                    with contextlib.suppress(OSError, Error):
                        connection.sendall(encoded(response))
            self.engine.cancel.set()
            self.wakeup.set()
            self.scheduler.join(timeout=90)
            if self.scheduler.is_alive():
                # Keep the lock and process alive until the job has actually
                # stopped; sign-out must never race an outstanding upload.
                self.scheduler.join()
        finally:
            self.stopping.set()
            self.engine.cancel.set()
            server.close()
            with contextlib.suppress(FileNotFoundError):
                self.paths.socket.unlink()


def receive_socket(connection, timeout=10):
    deadline = time.monotonic() + timeout
    data = bytearray()
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Error("The private sync request timed out. Try again.")
            connection.settimeout(remaining)
            chunk = connection.recv(min(4096, LIMIT + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > LIMIT:
                raise Error("The sync request exceeds the safety limit.")
            if b"\n" in chunk:
                if not data.endswith(b"\n") or data.count(b"\n") != 1:
                    raise Error("The sync request contains extra messages.")
                break
    except OSError:
        raise Error("The private sync request timed out.") from None
    return decode_json(data)


def rpc(paths, request, timeout=None):
    timeout = timeout if timeout is not None else (225 if request.get("action") == "stop" else 180)
    deadline = time.monotonic() + timeout
    paths.helper.safe_directory(paths.directory)
    try:
        info = paths.socket.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600:
            raise Error("The private sync socket is unsafe.")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(min(60, timeout))
            connection.connect(str(paths.socket))
            _pid, uid, _gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
            if uid != os.getuid():
                raise Error("The sync worker belongs to another user.")
            connection.sendall(encoded(request))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Error("The private sync worker request timed out. Try again.")
            return receive_socket(connection, timeout=remaining)
    except (FileNotFoundError, ConnectionRefusedError):
        raise Error("The Google Drive sync worker is not running.") from None
    except OSError:
        raise Error("The private sync worker is unavailable or busy.") from None


def public_status(paths, message="", ok=True):
    _binary, version, available = binary_version(paths.home)
    try:
        # A long, read-only folder check must not make capability or worker
        # identity disappear from another window's five-second status poll.
        response = rpc(paths, {"version": 1, "action": "status"}, timeout=2)
        if message or not ok:
            response.update(ok=ok, message=message)
        return response
    except Error:
        running = False
        if paths.lock_file.exists() or paths.lock_file.is_symlink():
            with MOUNT.checked_file(paths.lock_file, private=True) as stream:
                try:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    running = True
        state = paths.read_state()
        pairs = [public_pair(pair) for pair in state["pairs"]]
        if not available and not message:
            message = "Two-way sync requires a stable rclone 1.71.0 or newer; found " + version + "."
        response = {"version": 1, "ok": ok, "message": message, "worker_running": running, "rclone_version": version, "available": available,
                    "sync_configured": paths.config.exists() and MOUNT.encrypted_config(paths.config), "pairs": pairs}
        encoded(response)
        return response


def make_worker(paths, credential, binary, version):
    sync_password, client_id, client_secret = credential["config_password"], credential["client_id"], credential["client_secret"]
    engine = Rclone(paths, sync_password, binary)
    browse_token = engine.config_document(paths.helper.config, credential["browse_config_password"], "drive.readonly", credential["client_id"], credential["client_secret"])
    browse_account = DriveAPI(browse_token).account()
    engine.install(credential)
    token = engine.config_document(paths.config, credential["config_password"], "drive", credential["client_id"], credential["client_secret"])
    api = DriveAPI(token)
    api.cancel = engine.cancel
    if api.account() != browse_account:
        raise Error("The sync authorization belongs to a different Google account than the streamed Drive.")
    del browse_token
    worker = Worker(paths, engine, api, browse_account, version)
    # Refresh the metadata bearer through the encrypted rclone config before
    # every job; the worker never stores an unencrypted access or refresh token.
    original_run_pair = worker.run_pair
    def current_token(deadline=None):
        return engine.config_document(paths.config, sync_password, "drive", client_id, client_secret, deadline=deadline)
    api.refresh = current_token
    def refresh_and_run(pair):
        api.token = current_token()
        if api.account() != worker.account:
            raise Error("The Drive account identity changed. Synchronization was stopped.")
        return original_run_pair(pair)
    worker.run_pair = refresh_and_run
    return worker


def start(paths, credential):
    binary, version, available = binary_version(paths.home)
    if not available:
        raise Error("Two-way sync requires a stable rclone 1.71.0 or newer; found " + version + ".")
    with paths.lock():
        read_fd, write_fd = os.pipe2(os.O_CLOEXEC)
        child = os.fork()
        if child == 0:
            os.close(read_fd)
            try:
                os.setsid()
                null = os.open(os.devnull, os.O_RDWR)
                for fd in (0, 1, 2):
                    os.dup2(null, fd)
                if null > 2:
                    os.close(null)
                sent = False
                def ready(response):
                    nonlocal sent
                    os.write(write_fd, encoded(response))
                    sent = True
                    os.close(write_fd)
                worker = make_worker(paths, credential, binary, version)
                credential.clear()
                signal.signal(signal.SIGTERM, lambda _number, _frame: (worker.stopping.set(), worker.engine.cancel.set(), worker.wakeup.set()))
                signal.signal(signal.SIGINT, signal.SIG_IGN)
                worker.serve(ready)
            except BaseException as error:
                if not sent:
                    message = str(error) if isinstance(error, Error) else "The sync worker could not start safely."
                    with contextlib.suppress(OSError):
                        os.write(write_fd, encoded({"version": 1, "ok": False, "message": message, "worker_running": False, "rclone_version": version, "available": available, "pairs": []}))
            finally:
                os._exit(0)
        os.close(write_fd)
        data = bytearray()
        try:
            deadline = time.monotonic() + 120
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([read_fd], [], [], remaining)[0]:
                    os.kill(child, signal.SIGTERM)
                    raise Error("The sync worker did not become ready in time.")
                chunk = os.read(read_fd, min(4096, LIMIT + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > LIMIT:
                    os.kill(child, signal.SIGTERM)
                    raise Error("The sync worker returned an oversized startup response.")
            return decode_json(data)
        finally:
            os.close(read_fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "start", "request", "forget"))
    args = parser.parse_args(argv)
    if os.getuid() == 0:
        parser.error("Run the sync helper as your normal desktop user.")
    os.umask(0o077)
    paths = Paths()
    try:
        if args.command == "status":
            response = public_status(paths)
        elif args.command == "request":
            response = rpc(paths, read_stdin())
        elif args.command == "start":
            response = start(paths, credentials(read_stdin()))
        else:
            with paths.lock():
                if paths.config.exists() or paths.config.is_symlink():
                    if not MOUNT.encrypted_config(paths.config):
                        raise Error("The saved sync configuration is unsafe.")
                    paths.config.unlink()
            response = public_status(paths, "The encrypted sync authorization was removed. Local folders and pair settings were retained.")
        sys.stdout.buffer.write(encoded(response))
        return 0 if response.get("ok") else 1
    except Error as error:
        try:
            response = public_status(paths, str(error), False)
        except Error:
            response = {"version": 1, "ok": False, "message": str(error), "worker_running": False, "rclone_version": "unavailable", "available": False, "pairs": []}
        sys.stdout.buffer.write(encoded(response))
        return 1
    except Exception:
        # Unexpected filesystem/process failures must never print a traceback
        # or an unbounded child exception containing paths or credential data.
        sys.stdout.buffer.write(encoded({"version": 1, "ok": False, "message": "The sync operation could not complete safely.",
                                        "worker_running": True, "rclone_version": "unavailable", "available": False,
                                        "sync_configured": False, "pairs": []}))
        return 1


if __name__ == "__main__":
    sys.exit(main())
