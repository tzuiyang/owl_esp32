#!/usr/bin/env python3
"""
owl_watcher — watch an owl ESP32's gallery and alert on known faces.

Polls the device's HTTP API, downloads any new photos, runs face recognition
against a local known-faces database, and shows a macOS notification when a
match is detected.

Setup:
    1. Join the device's WiFi (SSID "owl", password "owlowlowl") on this Mac.
    2. pip install face_recognition requests
    3. Drop reference photos into ./known_faces/<person>/*.jpg, e.g.
           known_faces/
             alice/
               1.jpg
               2.jpg
             bob/
               portrait.jpg
    4. python watcher.py
"""

from __future__ import annotations
import argparse
import subprocess
import sys
import time
from pathlib import Path

import face_recognition
import requests

DEFAULT_HOST       = "owl.local"
DEFAULT_INTERVAL_S = 10
DEFAULT_TOLERANCE  = 0.6      # face_recognition's library default
KNOWN_FACES_DIR    = Path("known_faces")
IMAGE_EXTS         = {".jpg", ".jpeg", ".png"}


def load_known_faces(root: Path):
    """Encode every photo in known_faces/<name>/*.jpg into (encoding, name) pairs."""
    if not root.is_dir():
        sys.exit(f"error: {root}/ does not exist. Create it with subfolders per person.")
    encodings = []
    names = []
    for person_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        for img in sorted(person_dir.iterdir()):
            if img.suffix.lower() not in IMAGE_EXTS:
                continue
            try:
                image = face_recognition.load_image_file(str(img))
                encs = face_recognition.face_encodings(image)
            except Exception as e:
                print(f"  [skip] {img}: {e}", file=sys.stderr)
                continue
            if not encs:
                print(f"  [skip] {img}: no face detected", file=sys.stderr)
                continue
            encodings.append(encs[0])
            names.append(person_dir.name)
            print(f"  loaded {person_dir.name}: {img.name}")
    if not encodings:
        sys.exit(f"error: no usable faces in {root}/. Add photos and retry.")
    return encodings, names


def list_photos(host: str) -> list[str]:
    r = requests.get(f"http://{host}/list", timeout=5)
    r.raise_for_status()
    return r.json().get("photos", [])


def download_photo(host: str, name: str, dest: Path) -> bool:
    r = requests.get(f"http://{host}/photo/{name}", timeout=15, stream=True)
    if r.status_code != 200:
        return False
    with open(dest, "wb") as f:
        for chunk in r.iter_content(8192):
            f.write(chunk)
    return True


def match_photo(path: Path, known_encs, known_names, tolerance):
    """Return list of (name, distance) for every known face matched in this photo."""
    try:
        image = face_recognition.load_image_file(str(path))
    except Exception as e:
        print(f"  [error] cannot read {path.name}: {e}")
        return []
    encs = face_recognition.face_encodings(image)
    if not encs:
        return []
    matches = []
    for enc in encs:
        distances = face_recognition.face_distance(known_encs, enc)
        best_i = int(distances.argmin())
        if distances[best_i] <= tolerance:
            matches.append((known_names[best_i], float(distances[best_i])))
    return matches


def macos_notify(title: str, body: str) -> None:
    if sys.platform != "darwin":
        return
    subprocess.run(
        ["osascript", "-e", f'display notification {body!r} with title {title!r}'],
        check=False,
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Poll an owl ESP32 and alert when a known face is captured.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host", default=DEFAULT_HOST,
                    help=f"device hostname or IP (default {DEFAULT_HOST}; falls back to 192.168.4.1)")
    ap.add_argument("--interval", type=int, default=DEFAULT_INTERVAL_S,
                    help=f"polling interval, seconds (default {DEFAULT_INTERVAL_S})")
    ap.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE,
                    help=f"face-distance threshold (default {DEFAULT_TOLERANCE}; lower = stricter)")
    ap.add_argument("--known-dir", type=Path, default=KNOWN_FACES_DIR,
                    help=f"folder of known faces (default ./{KNOWN_FACES_DIR})")
    ap.add_argument("--cache-dir", type=Path, default=Path(".watcher_cache"),
                    help="local folder for downloaded photos")
    ap.add_argument("--catchup", action="store_true",
                    help="process all existing photos on startup (default: only new captures)")
    ap.add_argument("--no-notify", action="store_true",
                    help="suppress macOS notifications; terminal output only")
    args = ap.parse_args()

    print(f"loading known faces from {args.known_dir}/...")
    known_encs, known_names = load_known_faces(args.known_dir)
    print(f"  total: {len(known_encs)} encoding(s) for {len(set(known_names))} person/people\n")

    args.cache_dir.mkdir(exist_ok=True)
    seen: set[str] = set()
    if not args.catchup:
        try:
            seen = set(list_photos(args.host))
            print(f"skipping {len(seen)} existing photo(s) — only new captures will be processed")
        except Exception as e:
            print(f"[warn] couldn't fetch initial list: {e}", file=sys.stderr)
    print(f"polling http://{args.host}/list every {args.interval}s. Ctrl-C to stop.\n")

    while True:
        try:
            current = list_photos(args.host)
        except requests.RequestException as e:
            print(f"[warn] list failed: {e}", file=sys.stderr)
            time.sleep(args.interval)
            continue

        new = [n for n in current if n not in seen]
        for name in new:
            local = args.cache_dir / name
            if not download_photo(args.host, name, local):
                print(f"[warn] failed to download {name}", file=sys.stderr)
                continue
            seen.add(name)
            matches = match_photo(local, known_encs, known_names, args.tolerance)
            if not matches:
                print(f"  {name}  →  no known face")
                continue
            for who, dist in matches:
                print(f"  {name}  →  {who}  (dist={dist:.3f})")
                if not args.no_notify:
                    macos_notify("owl: known face seen", f"{who} in {name}")
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nstopped")
