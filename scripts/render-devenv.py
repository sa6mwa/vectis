#!/usr/bin/env python3
"""Render the checked-in Podman Kube graph for this checkout."""

import hashlib
import os
from pathlib import Path
import re
import secrets
import sys

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "build/devenv"
PORT_NAMES = (
    "MINIO_API", "MINIO_CONSOLE", "LOCKD_DISK", "LOCKD_S3", "SSH", "MQTT"
)


def ports():
    checkout = hashlib.sha256(str(ROOT).encode()).digest()
    base = 40000 + (int.from_bytes(checkout[:2], "big") % 1000) * 8
    result = {}
    for offset, name in enumerate(PORT_NAMES):
        key = "VECTIS_" + name + "_PORT"
        value = os.environ.get(key, str(base + offset))
        if not value.isdecimal() or not 1024 <= int(value) <= 65535:
            raise ValueError(f"{key} must be a non-privileged TCP port")
        result[name + "_PORT"] = str(int(value))
    if len(set(result.values())) != len(result):
        raise ValueError("Vectis service ports must be distinct")
    return result


def credentials():
    path = DEST / "credentials"
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    result = {}
    for name in ("MINIO_PASSWORD", "SSH_PASSWORD"):
        file = path / name.lower()
        if not file.exists():
            fd = os.open(file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as stream:
                stream.write(secrets.token_hex(20) + "\n")
        file.chmod(0o600)
        value = file.read_text().strip()
        if not re.fullmatch(r"[a-f0-9]{40}", value):
            raise ValueError(f"invalid generated credential: {file}")
        result[name] = value
    return result


def main():
    config = ports()
    if len(sys.argv) == 2 and sys.argv[1] == "env":
        for name, value in config.items():
            print(f"export VECTIS_{name}={value}")
        return
    if len(sys.argv) != 2 or sys.argv[1] != "render":
        raise SystemExit("usage: render-devenv.py env|render")
    DEST.mkdir(mode=0o700, parents=True, exist_ok=True)
    config.update(credentials())
    config["MINIO_USER"] = "vectisdev"
    config["NAME"] = "vectis-" + hashlib.sha256(str(ROOT).encode()).hexdigest()[:10]
    config["STATE"] = str(DEST / "state")
    config["ROOT"] = str(ROOT)
    for name in ("minio", "lockd-config", "lockd-disk", "ssh-config", "ssh-data"):
        (DEST / "state" / name).mkdir(mode=0o700, parents=True, exist_ok=True)
    template = (ROOT / "devenv.yaml.in").read_text()
    for key, value in config.items():
        template = template.replace("@" + key + "@", value)
    if re.search(r"@[A-Z_]+@", template):
        raise ValueError("unresolved devenv template placeholder")
    manifest = DEST / "devenv.yaml"
    write_private(manifest, template)
    docs = template.split("\n---\n")
    if len(docs) != 4:
        raise ValueError("expected four service pods")
    for name, doc in zip(("minio", "lockd", "ssh", "mqtt"), docs):
        path = DEST / (name + ".yaml")
        write_private(path, doc)
    print(f"manifest={manifest}")
    for image in dict.fromkeys(re.findall(r"^\s+image: (\S+)$", template, re.M)):
        print(f"image={image}")
    for name in ("minio", "lockd", "ssh", "mqtt"):
        print(f"pod={config['NAME']}-{name}")


def write_private(path, content):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(content)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"devenv render: {exc}") from exc
