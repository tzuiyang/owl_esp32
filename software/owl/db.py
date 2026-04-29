"""SQLite-backed people + encounters store.

Schema is created on first connect. People rows hold a 512-d face centroid
that's updated each time the same person is matched again; encounters are
keyed by session_dir so reprocessing the same session is a no-op.
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import numpy as np

SCHEMA = """
CREATE TABLE IF NOT EXISTS people (
    id              INTEGER PRIMARY KEY,
    face_embedding  BLOB    NOT NULL,
    face_crop_path  TEXT    NOT NULL,
    best_face_score REAL    NOT NULL,
    name            TEXT,
    first_met       TEXT    NOT NULL,
    last_met        TEXT    NOT NULL
);

CREATE TABLE IF NOT EXISTS encounters (
    id          INTEGER PRIMARY KEY,
    person_id   INTEGER NOT NULL REFERENCES people(id),
    session_dir TEXT    NOT NULL UNIQUE,
    started_at  TEXT    NOT NULL,
    audio_paths TEXT    NOT NULL DEFAULT '[]',
    transcript  TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_encounters_person ON encounters(person_id);
"""


def open_db(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.executescript(SCHEMA)
    _migrate(conn)
    return conn


def _migrate(conn: sqlite3.Connection) -> None:
    columns = {
        row[1]
        for row in conn.execute("PRAGMA table_info(encounters)").fetchall()
    }
    if "audio_paths" not in columns:
        conn.execute(
            "ALTER TABLE encounters "
            "ADD COLUMN audio_paths TEXT NOT NULL DEFAULT '[]'"
        )
        conn.commit()


def _emb_to_blob(emb: np.ndarray) -> bytes:
    return np.asarray(emb, dtype=np.float32).tobytes()


def _blob_to_emb(b: bytes) -> np.ndarray:
    return np.frombuffer(b, dtype=np.float32)


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def has_encounter(conn: sqlite3.Connection, session_dir: str) -> bool:
    return conn.execute(
        "SELECT 1 FROM encounters WHERE session_dir = ?", (session_dir,)
    ).fetchone() is not None


def match_or_create_person(
    conn: sqlite3.Connection,
    embedding: np.ndarray,
    crop_path: Path,
    score: float,
    *,
    threshold: float = 0.45,
    when: Optional[str] = None,
) -> tuple[int, bool, float]:
    """Match against existing centroids or insert a new person.

    Returns (person_id, created, similarity). similarity is the cosine sim to
    the matched person, or 1.0 for newly created rows.
    """
    when = when or _now_iso()
    rows = conn.execute(
        "SELECT id, face_embedding, best_face_score FROM people"
    ).fetchall()

    best_id: Optional[int] = None
    best_sim = -1.0
    best_existing_score = 0.0
    best_emb_blob: Optional[bytes] = None
    for pid, emb_blob, existing_score in rows:
        emb = _blob_to_emb(emb_blob)
        sim = float(np.dot(emb, embedding))
        if sim > best_sim:
            best_sim = sim
            best_id = pid
            best_existing_score = float(existing_score)
            best_emb_blob = emb_blob

    if best_id is not None and best_sim >= threshold:
        old = _blob_to_emb(best_emb_blob)
        new_centroid = old + embedding
        new_centroid = new_centroid / (np.linalg.norm(new_centroid) + 1e-9)
        if score > best_existing_score:
            conn.execute(
                "UPDATE people SET face_embedding=?, face_crop_path=?, "
                "best_face_score=?, last_met=? WHERE id=?",
                (_emb_to_blob(new_centroid), str(crop_path), score, when, best_id),
            )
        else:
            conn.execute(
                "UPDATE people SET face_embedding=?, last_met=? WHERE id=?",
                (_emb_to_blob(new_centroid), when, best_id),
            )
        conn.commit()
        return best_id, False, best_sim

    cur = conn.execute(
        "INSERT INTO people (face_embedding, face_crop_path, best_face_score, "
        "first_met, last_met) VALUES (?, ?, ?, ?, ?)",
        (_emb_to_blob(embedding), str(crop_path), score, when, when),
    )
    conn.commit()
    return int(cur.lastrowid), True, 1.0


def add_encounter(
    conn: sqlite3.Connection,
    person_id: int,
    session_dir: str,
    started_at: str,
    audio_paths: list[str],
    transcript: str,
) -> int:
    cur = conn.execute(
        "INSERT INTO encounters "
        "(person_id, session_dir, started_at, audio_paths, transcript) "
        "VALUES (?, ?, ?, ?, ?)",
        (person_id, session_dir, started_at, json.dumps(audio_paths), transcript),
    )
    conn.commit()
    return int(cur.lastrowid)


def list_people(conn: sqlite3.Connection) -> list[dict]:
    rows = conn.execute(
        "SELECT p.id, p.name, p.face_crop_path, p.first_met, p.last_met, "
        "       (SELECT COUNT(*) FROM encounters e WHERE e.person_id = p.id) AS n "
        "FROM people p ORDER BY p.last_met DESC"
    ).fetchall()
    return [
        {
            "id": pid,
            "name": name,
            "face_crop_path": crop,
            "first_met": first_met,
            "last_met": last_met,
            "encounter_count": n,
        }
        for (pid, name, crop, first_met, last_met, n) in rows
    ]


def get_person(conn: sqlite3.Connection, person_id: int) -> Optional[dict]:
    row = conn.execute(
        "SELECT id, name, face_crop_path, first_met, last_met "
        "FROM people WHERE id = ?", (person_id,)
    ).fetchone()
    if row is None:
        return None
    return {
        "id": row[0], "name": row[1], "face_crop_path": row[2],
        "first_met": row[3], "last_met": row[4],
    }


def get_encounters(conn: sqlite3.Connection, person_id: int) -> list[dict]:
    rows = conn.execute(
        "SELECT id, session_dir, started_at, audio_paths, transcript FROM encounters "
        "WHERE person_id = ? ORDER BY started_at ASC", (person_id,)
    ).fetchall()
    encounters = []
    for r in rows:
        try:
            audio_paths = json.loads(r[3] or "[]")
        except json.JSONDecodeError:
            audio_paths = []
        encounters.append({
            "id": r[0],
            "session_dir": r[1],
            "started_at": r[2],
            "audio_paths": audio_paths,
            "transcript": r[4],
        })
    return encounters
