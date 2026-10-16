#!/usr/bin/env python3
"""Read-only Google Drive mount bridge using official rclone encryption.

Explorer owns app OAuth and the desktop wallet; app-* commands use private stdin.
Legacy setup is for a private interactive terminal, never a captured tool session.
Dependencies: Python 3 (stdlib), rclone, fuse3/fusermount3, Explorer.
Legacy personal setup also requires kdialog.
No system/default rclone configuration is read or changed.
"""

import argparse
import contextlib
import csv
import datetime
import fcntl
import getpass
import grp
import io
import json
import os
from pathlib import Path
import pwd
import re
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

REMOTE = "explorer-google-drive:"
ENCRYPTED_HEADER = b"RCLONE_ENCRYPT_V0:"


class HelperError(Exception):
    """A safe, credential-free error for display."""


def clean_environment():
    # Ambient rclone options can override config, credentials, read-only mode,
    # logging, or remote identity. Never inherit any of them.
    return {key: value for key, value in os.environ.items()
            if not key.lstrip("_").upper().startswith("RCLONE_")}


def require_binary(name):
    result = shutil.which(name)
    if not result:
        raise HelperError(f"Required program is unavailable: {name}.")
    return result


def rclone_command_string(arguments):
    # rclone's SpaceSepList flag uses CSV with a space delimiter, not a shell.
    # Shell single-quote escaping breaks arguments containing spaces/quotes.
    output = io.StringIO()
    csv.writer(output, delimiter=" ", lineterminator="").writerow(arguments)
    return output.getvalue()


def private_primary_group(group_id):
    """Permit Ubuntu's user-private-group umask002 without changing ancestors."""
    try:
        user = pwd.getpwuid(os.getuid())
        if group_id != user.pw_gid or group_id != os.getgid():
            return False
        group = grp.getgrgid(group_id)
        return (group.gr_gid == group_id
                and set(group.gr_mem).issubset({user.pw_name})
                and not any(account.pw_gid == group_id and account.pw_uid != os.getuid()
                            for account in pwd.getpwall()))
    except Exception:
        # Unknown membership/failed NSS enumeration cannot establish privacy.
        return False


def checked_file(path, private=False):
    """Open only a regular, single-link, user-owned file, without following links."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    except OSError:
        raise HelperError("The requested file is missing, inaccessible, or a symbolic link.") from None
    info = os.fstat(fd)
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_nlink != 1 or (private and stat.S_IMODE(info.st_mode) != 0o600)):
        os.close(fd)
        raise HelperError("The requested file has unsafe ownership, links, or permissions.")
    return os.fdopen(fd, "rb")


def encrypted_config(path):
    with checked_file(path, private=True) as stream:
        # rclone prefixes encrypted configs with comments and blank lines.
        for _ in range(16):
            line = stream.readline(1024)
            if not line:
                break
            line = line.strip()
            if line and not line.startswith((b"#", b";")):
                return line == ENCRYPTED_HEADER
    return False


def desktop_client(path):
    with checked_file(path) as stream:
        try:
            data = stream.read(64 * 1024 + 1)
            if len(data) > 64 * 1024:
                raise ValueError()
            document = json.loads(data)
            installed = document["installed"]
            client_id = installed["client_id"]
            client_secret = installed["client_secret"]
        except (ValueError, KeyError, TypeError):
            raise HelperError("Choose the downloaded Google Desktop app client JSON file.") from None
    # Reject INI injection, unexpected endpoints, and arbitrary credential types.
    if (not isinstance(client_id, str) or not isinstance(client_secret, str)
            or not re.fullmatch(r"[A-Za-z0-9_-]+\.apps\.googleusercontent\.com", client_id)
            or not client_secret or len(client_secret) > 4096
            or not re.fullmatch(r"[A-Za-z0-9._~-]+", client_secret)):
        raise HelperError("The Desktop app client JSON has invalid client fields.")
    return client_id, client_secret


def decode_mount_field(value):
    return re.sub(r"\\(040|011|012|134)",
                  lambda match: chr(int(match.group(1), 8)), value)


def mounts_at(text, path):
    """Parse Linux mountinfo, decoding only its four specified octal escapes."""
    result = []
    for line in text.splitlines():
        fields = line.split()
        try:
            separator = fields.index("-")
            if separator < 6 or len(fields) < separator + 4:
                continue
            if decode_mount_field(fields[4]) == str(path):
                result.append({"fstype": decode_mount_field(fields[separator + 1]),
                               "source": decode_mount_field(fields[separator + 2]),
                               "options": set(fields[5].split(","))})
        except (ValueError, IndexError):
            continue
    return result


def recognized_mount(mounts):
    return (len(mounts) == 1 and mounts[0]["fstype"] == "fuse.rclone"
            and mounts[0]["source"] == REMOTE and "ro" in mounts[0]["options"]
            and "rw" not in mounts[0]["options"])


class DriveHelper:
    def __init__(self, home=None, environment=None):
        self.home = Path.home() if home is None else Path(home)
        if not self.home.is_absolute():
            raise HelperError("The home directory must be absolute.")
        environment = os.environ if environment is None else environment
        def xdg_root(name, fallback):
            value = environment.get(name, "")
            root = Path(value) if value and Path(value).is_absolute() else self.home / fallback
            if not root.is_relative_to(self.home) or ".." in root.parts:
                raise HelperError(f"{name} must be an absolute directory inside your home for this helper.")
            return root
        self.config_dir = xdg_root("XDG_CONFIG_HOME", ".config") / "linux-explorer-win7/google-drive"
        self.config = self.config_dir / "rclone.conf"
        self.mount = xdg_root("XDG_DATA_HOME", ".local/share") / "linux-explorer-win7/mounts/google-drive"

    def safe_directory(self, path, create=False, mode=0o700):
        try:
            relative = path.relative_to(self.home)
        except ValueError:
            raise HelperError("The managed directory must remain inside your home directory.") from None
        current = self.home
        for component in relative.parts:
            if component in (".", ".."):
                raise HelperError("Unsafe directory path.")
            current /= component
            try:
                info = current.lstat()
            except FileNotFoundError:
                if not create:
                    return False
                current.mkdir(mode=0o700)
                info = current.lstat()
            permissions = stat.S_IMODE(info.st_mode)
            if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
                    or permissions & 0o002
                    or (permissions & 0o020 and not private_primary_group(info.st_gid))):
                raise HelperError("A managed path is not a user-owned directory, or is a symbolic link.")
        if create:
            os.chmod(path, mode, follow_symlinks=False)
        return True

    @contextlib.contextmanager
    def operation_lock(self):
        self.safe_directory(self.config_dir, create=True)
        fd = os.open(self.config_dir / ".mount.lock",
                     os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
                raise HelperError("Unsafe operation lock.")
            os.fchmod(fd, 0o600)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise HelperError("Another Google Drive setup or mount operation is in progress.") from None
            yield
        finally:
            os.close(fd)

    def mount_records(self):
        with Path("/proc/self/mountinfo").open() as stream:
            text = stream.read(1024 * 1024 + 1)
        if len(text) > 1024 * 1024:
            raise HelperError("The system mount table exceeds the safety limit.")
        return mounts_at(text, self.mount)

    def require_encrypted(self):
        self.safe_directory(self.config_dir)
        if not encrypted_config(self.config):
            raise HelperError("A password-encrypted rclone configuration is required. Run setup first.")

    def password_command(self):
        return rclone_command_string([require_binary("kdialog"), "--title", "Explorer Google Drive",
                                      "--password", "Unlock your Explorer Google Drive configuration"])

    def rclone_args(self, *args, config=None, unlock=True):
        result = [require_binary("rclone"), "--config", str(config or self.config),
                  "--log-level", "ERROR"]
        if unlock:
            result += ["--ask-password=false", "--password-command", self.password_command()]
        return result + list(args)

    def run_private(self, args, timeout=30):
        # Never capture or print child output: OAuth/config commands may contain
        # tokens; network errors may contain private filenames or response data.
        try:
            subprocess.run(args, env=clean_environment(), check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           stdin=subprocess.DEVNULL, timeout=timeout)
        except (subprocess.SubprocessError, OSError):
            raise HelperError("The operation failed or timed out. No child output was logged.") from None

    def encrypt_new_config(self, pending, password=None, pass_fds=()):
        # rclone 1.60 has no `config encryption set` command. Constrain its
        # official config UI to the password branch; never give the user the
        # edit/new-remote menu before the encryption gate. Passwords travel only
        # through this private process's stdin and are never persisted here.
        if password is None:
            password = getpass.getpass("New Google Drive configuration encryption password: ")
            confirmation = getpass.getpass("Confirm the new encryption password: ")
        else:
            confirmation = password
        if (password != confirmation or not password.strip() or password != password.strip()
                or any(ord(character) < 0x20 or ord(character) == 0x7f for character in password)):
            raise HelperError("Passwords must match, be nonempty, and have no leading/trailing whitespace or control characters.")
        process = subprocess.Popen(self.rclone_args("config", config=pending, unlock=False),
                                   env=clean_environment(), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   pass_fds=pass_fds)
        def answer_after(prompt, answer):
            deadline = time.monotonic() + 15
            received = b""
            while prompt not in received:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
                    raise HelperError("rclone's configuration password prompt timed out.")
                data = os.read(process.stdout.fileno(), 4096)
                if not data or len(received) + len(data) > 32768:
                    raise HelperError("rclone's configuration password menu was unexpected.")
                received += data
            process.stdin.write(answer.encode("utf-8") + b"\n")
            process.stdin.flush()
        try:
            answer_after(b"e/n/d/r/c/s/q> ", "s")
            answer_after(b"a/q> ", "a")
            answer_after(b"Enter NEW configuration password:\npassword:", password)
            answer_after(b"Confirm NEW configuration password:\npassword:", confirmation)
            answer_after(b"c/u/q> ", "q")
            answer_after(b"e/n/d/r/c/s/q> ", "q")
            process.stdin.close()
            if process.wait(timeout=15) != 0:
                raise HelperError("rclone could not encrypt the new configuration.")
        except BaseException:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            raise
        finally:
            password = confirmation = ""
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()

    def setup(self, client_json=None, reconnect=False):
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            raise HelperError("Run setup yourself in a private interactive terminal.")
        with self.operation_lock():
            if self.mount_records():
                raise HelperError("Disconnect the managed mount before configuring its account.")
            if reconnect:
                self.require_encrypted()
            else:
                if self.config.exists() or self.config.is_symlink():
                    raise HelperError("Configuration already exists. Use setup --reconnect to authorize it again.")
                client_id, client_secret = desktop_client(Path(client_json))
                # Publish only an encrypted config; an interrupted password step
                # removes the private temporary plaintext client configuration.
                with tempfile.TemporaryDirectory(prefix=".setup-", dir=self.config_dir) as temporary:
                    pending = Path(temporary) / "rclone.conf"
                    fd = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                    with os.fdopen(fd, "w") as stream:
                        stream.write("[explorer-google-drive]\ntype = drive\nclient_id = " + client_id
                                     + "\nclient_secret = " + client_secret + "\nscope = drive.readonly\n")
                    del client_id, client_secret
                    print("Choose a NEW encryption password in the masked terminal prompts.")
                    print("rclone will encrypt the private configuration before browser authorization begins.")
                    try:
                        self.encrypt_new_config(pending)
                    except (subprocess.SubprocessError, OSError):
                        raise HelperError("Configuration password setup did not complete.") from None
                    os.chmod(pending, 0o600, follow_symlinks=False)
                    if not encrypted_config(pending):
                        raise HelperError("Encryption was not enabled; OAuth was not started.")
                    try:
                        os.link(pending, self.config, follow_symlinks=False)
                    except FileExistsError:
                        raise HelperError("A configuration appeared during setup; it was not replaced.") from None
                    pending.unlink()
            print("Unlock in the masked desktop prompt, then authorize read-only Drive access in your browser.")
            print("Keep the terminal private; rclone manages the browser flow and credentials.")
            try:
                subprocess.run(self.rclone_args("config", "reconnect", REMOTE,
                                               "--drive-scope", "drive.readonly"),
                               env=clean_environment(), check=True)
            except (subprocess.SubprocessError, OSError):
                raise HelperError("Authorization did not complete. The encrypted config is retained; retry setup --reconnect.") from None
            os.chmod(self.config, 0o600, follow_symlinks=False)
            self.require_encrypted()
            print("Encrypted account configuration saved. Run connect to mount read-only.")

    def mount_arguments(self, daemon=True):
        args = self.rclone_args("mount", REMOTE, str(self.mount), "--devname", REMOTE,
                                "--read-only", "--drive-scope", "drive.readonly",
                                "--umask", "077", "--dir-perms", "0700", "--file-perms", "0600",
                                "--vfs-cache-mode", "off", "--contimeout", "15s", "--timeout", "30s")
        if daemon:
            args += ["--daemon", "--daemon-wait", "40s"]
        return args

    def start_mount(self):
        args = self.mount_arguments()
        process = subprocess.Popen(args, env=clean_environment(), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if process.wait(timeout=90) != 0:
                raise HelperError("Google Drive could not mount. Check authorization or connectivity and retry.")
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            # Interrupt only our launcher. rclone's daemon launcher forwards
            # interrupts to its own daemon; never search for or kill other jobs.
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            raise HelperError("Mount startup was cancelled or timed out.") from None

    def wait_mount(self, present, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            records = self.mount_records()
            if (present and recognized_mount(records)) or (not present and not records):
                return
            if records and not recognized_mount(records):
                raise HelperError("Another or writable filesystem occupies the managed mountpoint.")
            if time.monotonic() >= deadline:
                raise HelperError("The mount state did not settle before the timeout.")
            time.sleep(0.1)

    def verify_listing(self):
        # A bounded subprocess prevents an unresponsive FUSE directory from
        # hanging the helper. No names or content are emitted.
        self.run_private([sys.executable, "-c",
                          "import os,sys\nwith os.scandir(sys.argv[1]) as entries: next(entries, None)",
                          str(self.mount)], timeout=35)

    def open_explorer(self):
        try:
            # A new primary Explorer remains alive with its window. Launch it
            # detached; waiting with a timeout would kill a healthy GUI process.
            subprocess.Popen([require_binary("explorer-win7"), "--map-google-drive-mount"],
                             env=clean_environment(), stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True, close_fds=True)
        except OSError:
            raise HelperError("Explorer could not start; the verified read-only mount remains available.") from None

    @contextlib.contextmanager
    def backing_directory(self, create=True):
        if not self.safe_directory(self.mount.parent, create=create):
            raise HelperError("The managed mount directory disappeared.")
        if not self.safe_directory(self.mount):
            if not create:
                raise HelperError("The managed mount directory disappeared.")
            self.mount.mkdir(mode=0o000)
        original = self.mount.lstat()
        cleanup_mode = stat.S_IMODE(original.st_mode)
        path_fd = None
        fd = None
        try:
            if self.mount_records():
                raise HelperError("The mount changed; its backing directory was not adopted.")
            # O_PATH opens even a mode000 directory. Use this stable inode
            # through procfs when temporarily granting access, so an intervening
            # mount cannot redirect chmod onto someone else's filesystem.
            path_fd = os.open(self.mount, os.O_PATH | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            opened = os.fstat(path_fd)
            if ((opened.st_dev, opened.st_ino) != (original.st_dev, original.st_ino)
                    or self.mount_records()):
                raise HelperError("The mount changed; its backing directory was not adopted.")
            descriptor_path = f"/proc/self/fd/{path_fd}"
            os.chmod(descriptor_path, 0o700)
            fd = os.open(descriptor_path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            if self.mount_records():
                raise HelperError("The mount changed; its backing directory was not adopted.")
            if os.listdir(fd):
                # Refuse to adopt a directory containing unrelated local files;
                # preserve its original permissions as well as its contents.
                raise HelperError("The managed mount directory must be empty.")
            cleanup_mode = 0o000
            yield fd
        finally:
            if fd is not None:
                os.fchmod(fd, cleanup_mode)
                os.close(fd)
            elif path_fd is not None:
                # Even when opening a readable descriptor failed, the O_PATH
                # reference still identifies the original directory safely.
                opened = os.fstat(path_fd)
                if (opened.st_dev, opened.st_ino) == (original.st_dev, original.st_ino):
                    os.chmod(f"/proc/self/fd/{path_fd}", cleanup_mode)
            if path_fd is not None:
                os.close(path_fd)

    def require_unlock_program(self):
        require_binary("kdialog")

    def connect(self, open_explorer=True):
        with self.operation_lock():
            self.connect_locked(open_explorer)

    def connect_locked(self, open_explorer):
        self.require_encrypted()
        require_binary("fusermount3")
        self.require_unlock_program()
        if open_explorer:
            require_binary("explorer-win7")
        if not Path("/dev/fuse").exists():
            raise HelperError("FUSE is unavailable in this session.")
        records = self.mount_records()
        if records:
            if not recognized_mount(records):
                raise HelperError("The managed path is occupied by another or writable filesystem.")
            self.verify_listing()
            if open_explorer:
                self.open_explorer()
            print("Google Drive is already connected read-only.")
            return
        with self.backing_directory() as fd:
            ready = False
            try:
                self.start_mount()
                self.wait_mount(True)
                # This descriptor names the underlying local directory,
                # even while FUSE covers its pathname. Protect it before
                # checking the remote, including when that check fails.
                os.fchmod(fd, 0o000)
                self.verify_listing()
                ready = True
                if open_explorer:
                    self.open_explorer()
                print("Google Drive connected read-only.")
            except BaseException:
                os.fchmod(fd, 0o000)
                if not ready and recognized_mount(self.mount_records()):
                    try:
                        self.run_private([require_binary("fusermount3"), "-u", str(self.mount)])
                        self.wait_mount(False)
                    except HelperError:
                        print("The read-only mount is still present; use disconnect when it is idle.", file=sys.stderr)
                raise

    def disconnect(self):
        with self.operation_lock():
            records = self.mount_records()
            if records:
                if not recognized_mount(records):
                    raise HelperError("Refusing to unmount an unrecognized or writable filesystem.")
                self.run_private([require_binary("fusermount3"), "-u", str(self.mount)])
                self.wait_mount(False)
            if self.safe_directory(self.mount):
                if self.mount_records():
                    raise HelperError("The mount changed during disconnect; its permissions were not touched.")
                with self.backing_directory(create=False):
                    if self.mount_records():
                        raise HelperError("The mount changed during disconnect; refusing to continue.")
            print("Google Drive disconnected. Its local backing directory is inaccessible.")

    def status(self):
        self.safe_directory(self.config_dir)
        self.safe_directory(self.mount.parent)
        records = self.mount_records()
        configured = False
        if self.config.exists() or self.config.is_symlink():
            configured = encrypted_config(self.config)
        if records and not recognized_mount(records):
            raise HelperError("An unrecognized or writable filesystem occupies the managed mountpoint.")
        backing_mode = None
        if not records and self.safe_directory(self.mount):
            backing_mode = f"{stat.S_IMODE(self.mount.lstat().st_mode):03o}"
        print(json.dumps({"encrypted_config": configured, "connected_read_only": bool(records),
                          "mountpoint": str(self.mount), "disconnected_backing_mode": backing_mode}))


APP_ENVELOPE_LIMIT = 64 * 1024


def app_credentials(stream=None):
    """Read one bounded, versioned JSON envelope; never display its contents."""
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError()
            result[key] = value
        return result

    try:
        if stream is not None:
            data = stream.read(APP_ENVELOPE_LIMIT + 1)
        else:
            data = bytearray()
            deadline = time.monotonic() + 30
            fd = sys.stdin.fileno()
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                    raise ValueError()
                chunk = os.read(fd, min(4096, APP_ENVELOPE_LIMIT + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > APP_ENVELOPE_LIMIT:
                    raise ValueError()
        if len(data) > APP_ENVELOPE_LIMIT:
            raise ValueError()
        document = json.loads(data, object_pairs_hook=unique_object,
                              parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        if (not isinstance(document, dict)
                or set(document) != {"version", "client_id", "client_secret", "token", "config_password"}
                or type(document["version"]) is not int or document["version"] != 1):
            raise ValueError()
        client_id, client_secret = document["client_id"], document["client_secret"]
        password, token = document["config_password"], document["token"]
        if (not isinstance(client_id, str)
                or not re.fullmatch(r"[A-Za-z0-9_-]{1,1024}\.apps\.googleusercontent\.com", client_id)
                or not isinstance(client_secret, str)
                or not re.fullmatch(r"[A-Za-z0-9._~-]{1,4096}", client_secret)
                or not isinstance(password, str)
                or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", password)):
            raise ValueError()
        if token is None:
            return document
        if (not isinstance(token, dict)
                or set(token) != {"access_token", "refresh_token", "token_type", "expiry"}):
            raise ValueError()
        for value in token.values():
            if (not isinstance(value, str) or not 0 < len(value) <= 16384
                    or any(ord(character) < 0x20 or ord(character) == 0x7f for character in value)):
                raise ValueError()
        if token["token_type"].lower() != "bearer":
            raise ValueError()
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?(?:Z|[+-]\d{2}:\d{2})", token["expiry"]):
            raise ValueError()
        datetime.datetime.fromisoformat(token["expiry"].replace("Z", "+00:00"))
        return document
    except (ValueError, TypeError, KeyError, UnicodeError, OverflowError, RecursionError):
        raise HelperError("The application credential envelope is invalid or incomplete.") from None


class AppDriveHelper(DriveHelper):
    """App OAuth owns authorization and its wallet owns the encryption key."""
    def __init__(self, home=None, environment=None):
        super().__init__(home, environment)
        self.config = self.config_dir / "app-rclone.conf"
        self.config_password = None

    def require_unlock_program(self):
        require_binary("cat")

    def password_command(self):
        # rclone connects this command's stdin to its own stdin. A one-shot
        # pipe carries the key; argv, environment and files never contain it.
        return rclone_command_string([require_binary("cat")])

    def install_app_config(self, credentials):
        if not hasattr(os, "memfd_create"):
            raise HelperError("This system cannot securely hand application credentials to rclone.")
        if self.config.exists() or self.config.is_symlink():
            self.require_encrypted()
        token = json.dumps(credentials["token"], ensure_ascii=True, separators=(",", ":"))
        content = ("[explorer-google-drive]\ntype = drive\nscope = drive.readonly\n"
                   "client_id = " + credentials["client_id"] + "\nclient_secret = "
                   + credentials["client_secret"] + "\ntoken = " + token + "\n").encode("utf-8")
        # rclone 1.60's official writer encrypts in memory before creating its
        # on-disk configuration. Its loader needs a seekable file: sealed memfd
        # provides one without a plaintext temporary file. The controlled
        # pending symlink is replaced by rclone's encrypted regular file.
        fd = os.memfd_create("explorer-drive-config", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(os.dup(fd), "wb") as stream:
                stream.write(content)
            del content, token
            fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                        fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
            with tempfile.TemporaryDirectory(prefix=".app-setup-", dir=self.config_dir) as temporary:
                pending = Path(temporary) / "rclone.conf"
                pending.symlink_to(f"/proc/self/fd/{fd}")
                self.encrypt_new_config(pending, password=credentials["config_password"], pass_fds=(fd,))
                if not encrypted_config(pending):
                    raise HelperError("rclone did not produce an encrypted application configuration.")
                os.replace(pending, self.config)
                self.require_encrypted()
        finally:
            os.close(fd)

    def read_app_token(self, credentials):
        self.require_encrypted()
        with checked_file(self.config, private=True) as stream:
            if os.fstat(stream.fileno()).st_size > 256 * 1024:
                raise HelperError("The encrypted application configuration exceeds the safety limit.")
        try:
            result = subprocess.run(self.rclone_args("config", "dump"), env=clean_environment(),
                                    input=credentials["config_password"].encode("ascii") + b"\n",
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True, timeout=30)
            document = json.loads(result.stdout)
            if not isinstance(document, dict) or set(document) != {REMOTE[:-1]}:
                raise ValueError()
            remote = document[REMOTE[:-1]]
            if (not isinstance(remote, dict)
                    or set(remote) != {"type", "scope", "client_id", "client_secret", "token"}
                    or remote["type"] != "drive" or remote["scope"] != "drive.readonly"
                    or remote["client_id"] != credentials["client_id"]
                    or remote["client_secret"] != credentials["client_secret"]):
                raise ValueError()
            # Reuse the same strict token validation for disk-originated values.
            envelope = dict(credentials, token=json.loads(remote["token"]))
            token = app_credentials(io.BytesIO(json.dumps(envelope).encode("utf-8")))["token"]
            if token is None:
                raise ValueError()
            return token
        except (subprocess.SubprocessError, ValueError, TypeError, KeyError, UnicodeError,
                RecursionError, OverflowError, HelperError):
            raise HelperError("The application configuration could not be unlocked or does not match this application.") from None

    def app_forget(self):
        with self.operation_lock():
            if self.mount_records():
                raise HelperError("Disconnect Google Drive before removing the application's account configuration.")
            if self.config.exists() or self.config.is_symlink():
                self.require_encrypted()
                self.config.unlink()
        print("The application's Google Drive configuration was removed.")

    def app_export_token(self, credentials):
        if credentials["token"] is not None:
            raise HelperError("Token export requires an existing application configuration.")
        with self.operation_lock():
            token = self.read_app_token(credentials)
            # This command is a private app-to-helper protocol for revocation.
            # Its caller must never forward stdout to UI, diagnostics or logs.
            print(json.dumps(token, ensure_ascii=True, separators=(",", ":")))

    def app_connect(self, credentials, open_explorer=True):
        with self.operation_lock():
            records = self.mount_records()
            if records:
                # Preserve an active session rather than silently switching its
                # account or encryption key beneath an existing mount process.
                raise HelperError("Disconnect the existing Google Drive mount before signing in again.")
            if credentials["token"] is None:
                self.read_app_token(credentials)
            else:
                self.install_app_config(credentials)
            self.config_password = credentials["config_password"]
            try:
                self.connect_locked(open_explorer)
            finally:
                self.config_password = None

    def start_mount(self):
        if self.config_password is None:
            raise HelperError("The application must supply the configuration key.")
        # Do not use --daemon: rclone 1.60's daemon handoff writes a recoverable
        # encryption key to disk. A foreground process in a new session can
        # outlive this helper while keeping all key material in process memory.
        process = subprocess.Popen(self.mount_arguments(daemon=False), env=clean_environment(),
                                   stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
        try:
            process.stdin.write(self.config_password.encode("ascii") + b"\n")
            process.stdin.close()
            deadline = time.monotonic() + 45
            while process.poll() is None:
                records = self.mount_records()
                if recognized_mount(records):
                    return
                if records or time.monotonic() >= deadline:
                    break
                time.sleep(0.1)
            raise HelperError("Google Drive could not mount. Check authorization or connectivity and retry.")
        except BaseException:
            if not process.stdin.closed:
                process.stdin.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    setup = subparsers.add_parser("setup", help="Encrypt a dedicated config, then authorize in your browser")
    choice = setup.add_mutually_exclusive_group(required=True)
    choice.add_argument("--client-json", type=Path, help="Explicit downloaded Desktop OAuth client JSON")
    choice.add_argument("--reconnect", action="store_true", help="Reauthorize the existing encrypted config")
    connect = subparsers.add_parser("connect", help="Mount read-only and open Explorer")
    connect.add_argument("--no-open", action="store_true", help="Let the calling Explorer window open the verified mount")
    app_connect = subparsers.add_parser("app-connect", help="Mount using app-owned credentials supplied only on stdin")
    app_connect.add_argument("--no-open", action="store_true")
    subparsers.add_parser("app-forget", help="Remove only the disconnected encrypted application configuration")
    subparsers.add_parser("app-export-token", help="Return the current token privately to the calling application")
    subparsers.add_parser("app-status", help="Report app configuration and mount state without unlocking")
    subparsers.add_parser("disconnect", help="Unmount this helper's read-only mount without force")
    subparsers.add_parser("status", help="Report configuration/mount state without unlocking")
    args = parser.parse_args(argv)
    if os.getuid() == 0:
        parser.error("Run this helper as your normal desktop user, not root.")
    previous_umask = os.umask(0o077)
    def interrupted(_number, _frame):
        raise KeyboardInterrupt()
    previous_sigterm = signal.signal(signal.SIGTERM, interrupted)
    try:
        helper = AppDriveHelper() if args.command.startswith("app-") else DriveHelper()
        if args.command == "app-connect":
            helper.app_connect(app_credentials(), open_explorer=not args.no_open)
        elif args.command == "app-forget":
            helper.app_forget()
        elif args.command == "app-export-token":
            helper.app_export_token(app_credentials())
        elif args.command == "app-status":
            helper.status()
        elif args.command == "setup":
            helper.setup(args.client_json, args.reconnect)
        elif args.command == "connect":
            helper.connect(open_explorer=not args.no_open)
        else:
            getattr(helper, args.command)()
        return 0
    except (HelperError, OSError):
        # Never interpolate subprocess exceptions or JSON contents into errors.
        error = sys.exc_info()[1]
        print(str(error) if isinstance(error, HelperError)
              else "A filesystem operation failed; check ownership and permissions.", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("Cancelled.", file=sys.stderr)
        return 130
    finally:
        os.umask(previous_umask)
        signal.signal(signal.SIGTERM, previous_sigterm)


if __name__ == "__main__":
    sys.exit(main())
