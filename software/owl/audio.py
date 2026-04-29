"""Speech-to-text for one session.

Walks `<session>/audio/rec_*.wav` in numeric order, runs faster-whisper on
each clip, and joins the results with a blank line between clips.

The Whisper model is lazy-loaded once per process. First load downloads the
"small.en" weights (~244 MB) into the user's HuggingFace cache.
"""

from __future__ import annotations

from pathlib import Path
from threading import Lock

_model = None
_model_lock = Lock()


def _get_model(model_size: str = "small.en"):
    global _model
    with _model_lock:
        if _model is None:
            from faster_whisper import WhisperModel
            # int8 on CPU is the recommended fast/lean setting on Apple Silicon.
            _model = WhisperModel(model_size, device="cpu", compute_type="int8")
        return _model


def _transcribe_one(wav_path: Path) -> str:
    model = _get_model()
    segments, _info = model.transcribe(str(wav_path), language="en")
    return " ".join(seg.text.strip() for seg in segments).strip()


def audio_files_for_session(session_dir: Path) -> list[Path]:
    """Return rec_*.wav files in recording order."""
    audio_dir = session_dir / "audio"
    if not audio_dir.is_dir():
        return []
    return sorted(audio_dir.glob("rec_*.wav"))


def transcribe_session(session_dir: Path) -> str:
    """Return concatenated transcript for all rec_*.wav clips in this session."""
    wavs = audio_files_for_session(session_dir)
    if not wavs:
        return ""
    parts: list[str] = []
    for wav in wavs:
        text = _transcribe_one(wav)
        if text:
            parts.append(text)
    return "\n\n".join(parts)
