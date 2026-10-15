#!/usr/bin/env python3
"""Opt in to only Explorer's FileChooser portal, with guarded user-local rollback.

This edits configuration only. It never restarts a portal, KWin, or an application.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import time
import uuid

BACKEND = "explorer-win7"
BUS_NAME = "org.freedesktop.impl.portal.desktop.explorerwin7"
INTERFACE = "org.freedesktop.impl.portal.FileChooser"


def home_dir(variable, fallback):
    return Path(os.environ.get(variable) or fallback).expanduser().absolute()


def config_root():
    return home_dir("XDG_CONFIG_HOME", Path.home() / ".config") / "xdg-desktop-portal"


def state_root():
    return home_dir("XDG_STATE_HOME", Path.home() / ".local/state") / "linux-explorer-win7/portal-selection"


def desktops():
    return [name.lower() for name in os.environ.get("XDG_CURRENT_DESKTOP", "").split(":")
            if re.fullmatch(r"[A-Za-z0-9_-]+", name)]


def config_names():
    return [name + "-portals.conf" for name in desktops()] + ["portals.conf"]


def unique_paths(paths):
    result = []
    for path in paths:
        path = Path(path)
        if path.is_absolute() and path not in result:
            result.append(path)
    return result


def data_roots():
    return unique_paths([home_dir("XDG_DATA_HOME", Path.home() / ".local/share")]
        + (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":")
        + ["/usr/share"])


def config_roots():
    return unique_paths([config_root()]
        + [Path(p) / "xdg-desktop-portal" for p in
           (os.environ.get("XDG_CONFIG_DIRS") or "/etc/xdg").split(":") if p]
        + [Path("/etc/xdg-desktop-portal")]
        + [p / "xdg-desktop-portal" for p in data_roots()])


def first_config(root):
    for name in config_names():
        path = root / name
        if path.is_file():
            return path
    return None


def effective_configs():
    return [path for root in config_roots() if (path := first_config(root)) is not None]


def parse(text):
    parser = configparser.ConfigParser(interpolation=None, delimiters=("=",), strict=True)
    parser.optionxform = str
    parser.read_string(text.lstrip("\ufeff"))
    if parser.defaults():
        raise ValueError("Refusing INI DEFAULT inheritance, which portal key files do not use.")
    return parser


def read_text(path):
    return path.read_bytes().decode("utf-8")


def preferred(text):
    parser = parse(text)
    return dict(parser.items("preferred")) if parser.has_section("preferred") else {}


def patch_preference(text, value):
    """Change/remove exactly this key, preserving all other lines and comments."""
    parse(text)  # Refuse malformed/ambiguous input instead of rewriting it.
    lines = text.splitlines(keepends=True)
    newline = "\r\n" if "\r\n" in text else "\n"
    section = None
    insertion = None
    key_line = None
    for index, line in enumerate(lines):
        match = re.match(r"^\s*\[([^]]+)\]", line.lstrip("\ufeff"))
        if match:
            if section == "preferred" and insertion is None:
                insertion = index
            section = match.group(1)
        if section == "preferred":
            if re.match(r"^\s*" + re.escape(INTERFACE) + r"\s*=", line):
                key_line = index
    if key_line is not None:
        lines[key_line] = INTERFACE + "=" + value + newline if value is not None else ""
    elif value is not None:
        parser = parse(text)
        new_line = INTERFACE + "=" + value + newline
        if not parser.has_section("preferred"):
            if lines and not lines[-1].endswith(("\r", "\n")):
                lines[-1] += newline
            lines.extend([newline, "[preferred]" + newline, new_line])
        else:
            if insertion is None:
                insertion = len(lines)
            if insertion and not lines[insertion - 1].endswith(("\r", "\n")):
                lines[insertion - 1] += newline
            lines.insert(insertion, new_line)
    return "".join(lines)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data, mode=0o600):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise ValueError(f"Refusing to replace a symlink: {path}")
    fd, temporary = tempfile.mkstemp(prefix=".explorer-portal-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            os.fchmod(output.fileno(), mode)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def check_metadata(prefix):
    share = (prefix / "share").resolve()
    if share not in [p.resolve() for p in data_roots()]:
        raise ValueError("The install prefix's share directory is not in this session's XDG data search path. "
                         "Do not enable an undiscoverable backend.")
    metadata = share / "xdg-desktop-portal/portals" / (BACKEND + ".portal")
    parser = parse(read_text(metadata))
    interfaces = {s for s in parser.get("portal", "Interfaces").split(";") if s}
    if parser.get("portal", "DBusName") != BUS_NAME or interfaces != {INTERFACE}:
        raise ValueError("Installed metadata must declare only Explorer's FileChooser backend.")


def enable(prefix):
    if os.environ.get("XDG_DESKTOP_PORTAL_DIR"):
        raise ValueError("XDG_DESKTOP_PORTAL_DIR overrides normal discovery/configuration; unset it deliberately before enabling.")
    check_metadata(prefix)
    target = first_config(config_root())
    sources = effective_configs()
    source = target or (sources[0] if sources else None)
    if source is None:
        raise ValueError("No existing portal profile was found. Refusing to invent defaults for other interfaces.")
    if target is not None and target.is_symlink():
        raise ValueError(f"Portal preferences are symlink-managed; edit them deliberately instead: {target}")
    original = read_text(source)
    preferences = preferred(original)
    previous_value = preferences.get(INTERFACE)
    if previous_value and previous_value.split(";")[0].strip() == BACKEND:
        return {"changed": False, "configured": True, "config_file": str(target or source)}
    if target is None:
        target = config_root() / config_names()[0]
    before = target.read_bytes() if target.exists() else None
    fallback = previous_value or preferences.get("default", "")
    providers = [BACKEND] + [p.strip() for p in fallback.split(";") if p.strip() and p.strip() != BACKEND]
    value = ";".join(dict.fromkeys(providers)) + ";"
    after = patch_preference(original, value).encode("utf-8")
    mode = stat.S_IMODE(target.stat().st_mode) if before is not None else 0o600
    transaction = state_root() / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    transaction.mkdir(parents=True, mode=0o700)
    if before is not None:
        atomic_write(transaction / "before.conf", before, 0o600)
    atomic_write(transaction / "baseline.conf", original.encode("utf-8"), 0o600)
    manifest = {"target": str(target), "source": str(source), "created": before is None,
                "before_sha256": digest(before) if before is not None else None,
                "after_sha256": digest(after), "mode": mode,
                "previous_value": previous_value, "value": value}
    atomic_write(transaction / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode(), 0o600)
    atomic_write(target, after, mode)
    return {"changed": True, "configured": True, "config_file": str(target),
            "filechooser": value, "backup": str(transaction),
            "restart_required": "Only restart the portal frontend when no portal requests/sessions are active; a new login also applies it."}


def restore(backup=None):
    if backup is None:
        candidates = sorted(state_root().glob("*/manifest.json"), key=lambda path: path.stat().st_mtime_ns)
        if not candidates:
            raise ValueError("No Explorer portal preference backup exists.")
        backup = candidates[-1].parent
    manifest = json.loads((backup / "manifest.json").read_text())
    target = Path(manifest["target"])
    if target.parent != config_root() or target.name not in config_names():
        raise ValueError("Backup does not refer to this desktop's portal configuration.")
    if target.is_symlink():
        raise ValueError("Portal preferences became symlink-managed; refusing to replace them.")
    if not target.exists():
        return {"changed": False, "configured": False, "config_file": str(target)}
    current = target.read_bytes()
    values = preferred(current.decode("utf-8"))
    if values.get(INTERFACE) != manifest["value"]:
        raise ValueError("FileChooser selection was changed after enable; refusing to overwrite it.")
    if digest(current) == manifest["after_sha256"]:
        if manifest["created"]:
            target.unlink()
        else:
            before = (backup / "before.conf").read_bytes()
            if digest(before) != manifest["before_sha256"]:
                raise ValueError("Backup integrity check failed.")
            atomic_write(target, before, manifest["mode"])
    else:
        # Keep subsequent unrelated user edits; restore only this selection.
        after = patch_preference(current.decode("utf-8"), manifest["previous_value"])
        atomic_write(target, after.encode("utf-8"), stat.S_IMODE(target.stat().st_mode))
    return {"changed": True, "configured": False, "config_file": str(target), "backup": str(backup),
            "restart_required": "An idle portal frontend restart or new login applies the restored selection."}


def status():
    sources = effective_configs()
    preferences = preferred(read_text(sources[0])) if sources else {}
    value = preferences.get(INTERFACE)
    return {"configured": bool(value and value.split(";")[0].strip() == BACKEND),
            "config_file": str(sources[0]) if sources else None, "filechooser": value,
            "other_preferences": {k: v for k, v in preferences.items() if k != INTERFACE},
            "running_backend_not_checked": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("enable", "restore", "status"))
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local")
    parser.add_argument("--backup", type=Path)
    arguments = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run as the desktop user, not root.")
    try:
        result = enable(arguments.prefix.absolute()) if arguments.action == "enable" else \
                 restore(arguments.backup) if arguments.action == "restore" else status()
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, configparser.Error, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
