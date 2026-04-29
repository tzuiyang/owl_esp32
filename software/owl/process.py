"""Orchestrate one session: face → audio → DB upsert.

Idempotent on session_dir: if an encounter already exists for that path,
the session is skipped without re-running detection or transcription.
"""

from __future__ import annotations

import sqlite3
from datetime import datetime, timezone
from pathlib import Path

import cv2
from insightface.app import FaceAnalysis

from .audio import audio_files_for_session, transcribe_session
from .db import add_encounter, has_encounter, match_or_create_person
from .face import find_best_face


def _session_started_at(session_dir: Path) -> str:
    """Best-effort timestamp for the session."""
    try:
        ts = session_dir.stat().st_mtime
        return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat(timespec="seconds")
    except OSError:
        return datetime.now(timezone.utc).isoformat(timespec="seconds")


def process_session(
    session_dir: Path,
    app: FaceAnalysis,
    conn: sqlite3.Connection,
    *,
    padding: float = 0.25,
    min_confidence: float = 0.5,
    identity_threshold: float = 0.35,
    person_match_threshold: float = 0.45,
    jpeg_quality: int = 95,
    transcribe: bool = True,
) -> bool:
    session_key = str(session_dir.resolve())
    if has_encounter(conn, session_key):
        print("  [skip] already in db")
        return False

    frames_dir = session_dir / "image"
    if not frames_dir.is_dir():
        print("  [skip] no image/ subfolder")
        return False

    face = find_best_face(
        frames_dir, app,
        padding=padding,
        min_confidence=min_confidence,
        identity_threshold=identity_threshold,
    )
    if face is None:
        print("  [skip] no face detected")
        return False

    crop_path = session_dir / "best_face.jpg"
    cv2.imwrite(str(crop_path), face.crop, [int(cv2.IMWRITE_JPEG_QUALITY), jpeg_quality])

    audio_paths = [str(p.resolve()) for p in audio_files_for_session(session_dir)]
    transcript = transcribe_session(session_dir) if transcribe else ""

    started_at = _session_started_at(session_dir)
    person_id, created, sim = match_or_create_person(
        conn, face.embedding, crop_path, face.score,
        threshold=person_match_threshold, when=started_at,
    )
    add_encounter(conn, person_id, session_key, started_at, audio_paths, transcript)

    label = "NEW" if created else f"match person #{person_id} (sim={sim:.2f})"
    print(
        f"  -> {label}; best_face={crop_path.name} "
        f"(quality={face.score:.2f} from {face.src_frame.name}); "
        f"audio={len(audio_paths)} file(s); transcript={len(transcript)} chars"
    )
    return True
