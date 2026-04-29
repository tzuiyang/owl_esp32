# Owl pipeline — face + transcript per session

For each recording session on the SD card, pick the best face image of the
main person, transcribe everything that was said, and upsert both into a
local SQLite "people I've met" database. Same person across sessions =
same row, with a list of encounters underneath.

## Workflow

```
Session folders (one per recording)
        │
        ▼
  image/  frames  ──►  detect faces (InsightFace SCRFD)
                              │
                              ▼
                      embed each face (512-d)
                              │
                              ▼
                     cluster by cosine similarity
                              │
                              ▼
                   take the LARGEST cluster = main person of session
                              │
                              ▼
                    score each crop in that cluster
                    (sharpness 40% · frontality 30%
                     · size 20% · det confidence 10%)
                              │
                              ▼
                    save best crop  ◄────► match against people.face_embedding
                                                 │
                                                 ▼
                                        existing row → update centroid
                                                  + (maybe) replace crop
                                        no match    → insert new person

  audio/  rec_*.wav  ──►  faster-whisper (small.en)  ──►  joined transcript
                                                                │
                                                                ▼
                                      encounters row (person_id, audio_paths, transcript)
```

## Input layout

The firmware writes one folder per recording containing `image/` (JPEGs every
2 s) and `audio/` (normally one WAV per BOOT-tap start/stop cycle). Session folder
names are arbitrary — the pipeline only requires an `image/` subfolder.

```
data/
├── 2026-04-22_001/
│   ├── image/
│   │   ├── img_000001.jpg
│   │   ├── img_000002.jpg
│   │   └── ...
│   └── audio/
│       └── rec_001.wav
├── 2026-04-22_002/
│   ├── image/
│   └── audio/
└── ...
```

Supported frame extensions: `.jpg .jpeg .png .bmp .webp`.

## Output

Two things land per session:

- `best_face.jpg` written into the session folder (the cropped face).
- A row in `encounters` and either a new or updated row in `people` inside
  the SQLite DB (default location: `<input-dir>/owl.sqlite`). The encounter
  stores the transcript plus the absolute paths of the WAV files used.

Reprocessing the same `--input-dir` is safe: sessions whose `session_dir`
already exists in `encounters` are skipped.

## Running

```bash
# First-time setup
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

# Process a directory of sessions
.venv/bin/python -m owl process --input-dir ./data

# Browse the DB
.venv/bin/python -m owl list
.venv/bin/python -m owl show 1
```

Per-session console output:

```
[session] 2026-04-22_001
  -> NEW; best_face=best_face.jpg (quality=0.78 from img_000147.jpg); transcript=812 chars
[session] 2026-04-22_002
  -> match person #1 (sim=0.62); best_face=best_face.jpg (quality=0.83 from img_000034.jpg); transcript=540 chars
```

## Tuning knobs (`owl process`)

| Flag | Default | When to change |
| --- | --- | --- |
| `--padding` | `0.25` | Looser crop (more hair/shoulders). |
| `--min-confidence` | `0.5` | Raise to drop weak detections before clustering. |
| `--identity-threshold` | `0.35` | Intra-session cluster tightness. |
| `--person-match-threshold` | `0.45` | Cross-session identity. Lower = merge more aggressively across encounters. |
| `--det-size` | `640` | Larger (e.g. 960) helps with small/distant faces. |
| `--no-transcribe` | _off_ | Skip Whisper for face-only debugging. |
| `--db-path` | `<input-dir>/owl.sqlite` | Override the SQLite path. |

## DB schema

```sql
people (
  id INTEGER PRIMARY KEY,
  face_embedding BLOB,        -- centroid of every embedding seen, L2-normalised
  face_crop_path TEXT,        -- best crop seen so far
  best_face_score REAL,       -- the score that produced face_crop_path
  name TEXT,                  -- nullable; not yet user-editable from CLI
  first_met TEXT,
  last_met TEXT
)

encounters (
  id INTEGER PRIMARY KEY,
  person_id INTEGER REFERENCES people(id),
  session_dir TEXT UNIQUE,    -- absolute path; idempotency key
  started_at TEXT,            -- session folder mtime
  audio_paths TEXT,            -- JSON list of absolute WAV paths
  transcript TEXT             -- joined from all rec_*.wav, blank line between clips
)
```

You can poke at it directly:

```bash
sqlite3 data/owl.sqlite "SELECT id, name, last_met FROM people"
```

## Dependencies

- Python 3.12 (repo is set up in `.venv/`)
- `opencv-python` — image I/O, sharpness metric
- `numpy` — clustering math, embedding storage
- `insightface` — face detection + 512-d recognition embeddings
- `onnxruntime` — runs the InsightFace ONNX models on CPU
- `faster-whisper` — speech-to-text (CTranslate2 backend, runs on CPU)

First runs auto-download:

- InsightFace `buffalo_l` (~280 MB) → `~/.insightface/models/`
- faster-whisper `small.en` (~244 MB) → HuggingFace cache

Cached after that; runs offline.

## Files

- `owl/face.py` — detection, intra-session clustering, best-crop scoring.
- `owl/audio.py` — faster-whisper transcription (lazy-loaded model).
- `owl/db.py` — SQLite schema + person matching / encounter upsert.
- `owl/process.py` — orchestrates one session end-to-end.
- `owl/__main__.py` — `process` / `list` / `show` CLI.
- `extract_faces.py` — legacy face-only entrypoint, retained for compatibility.
- `requirements.txt` — pinned dependency floor.
- `data/` — drop session folders here.
- `.venv/` — Python virtualenv (not committed).

## Quality-score details (used to pick best_face.jpg)

Inside the main-person cluster, every candidate crop is scored on four
features, min-max normalised across the cluster:

1. **Sharpness** — variance of the Laplacian on the grayscale crop.
2. **Frontality** — nose offset relative to eye/mouth axes, divided by
   inter-eye distance. ~1 = looking at the camera.
3. **Size** — pixel area of the face bounding box.
4. **Detector confidence** — InsightFace's own score.

Weighted sum (`0.4·sharp + 0.3·front + 0.2·size + 0.1·det`) is the ranking
key; the top crop is saved with padding applied. The same score is used as
`best_face_score` in `people` so a crisper future encounter can replace
the stored crop.

## What's NOT here yet

- Summarisation (LLM notes from the transcript). An additive `summary` column
  will be added when an LLM stage lands.
- CLI for naming people (`owl rename <id> "Alice"`).
- Cross-encounter merge / split tools when identity matching gets it wrong.
