"""Face detection + best-crop selection for one session.

Lifted from the original extract_faces.py: detect every face in every frame,
cluster within the session by 512-d recognition embedding, take the largest
cluster as the "main person" of the session, then pick the highest-quality
crop from that cluster.

Returns a FaceResult instead of writing a file — the caller (process.py) is
responsible for persisting the crop and threading the embedding into the DB.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from insightface.app import FaceAnalysis

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


@dataclass
class FaceResult:
    crop: np.ndarray          # padded BGR crop, ready to imwrite
    embedding: np.ndarray     # 512-d L2-normalized identity embedding
    score: float              # composite quality score in [0, 1]
    src_frame: Path
    sharpness: float
    frontality: float
    size: int
    det_score: float


def _iter_frames(frames_dir: Path):
    for p in sorted(frames_dir.rglob("*")):
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS:
            yield p


def _sharpness(bgr) -> float:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    return float(cv2.Laplacian(gray, cv2.CV_64F).var())


def _frontality_from_kps(kps: np.ndarray) -> float:
    # InsightFace kps order: left_eye, right_eye, nose, left_mouth, right_mouth.
    # Frontal => nose x sits between eye-midpoint and mouth-midpoint.
    le, re, nose, lm, rm = kps
    face_axis_x = ((le[0] + re[0]) / 2 + (lm[0] + rm[0]) / 2) / 2
    horizontal_dev = abs(float(nose[0]) - float(face_axis_x))
    face_width = float(np.linalg.norm(re - le)) + 1e-9
    return 1.0 / (1.0 + horizontal_dev / face_width)


def _cluster_embeddings(embeddings: list[np.ndarray], threshold: float):
    """Greedy cosine-similarity clustering. Embeddings assumed L2-normalized."""
    clusters: list[dict] = []
    for i, emb in enumerate(embeddings):
        best_j = -1
        best_sim = -1.0
        for j, c in enumerate(clusters):
            sim = float(np.dot(emb, c["centroid"]))
            if sim > best_sim:
                best_sim = sim
                best_j = j
        if best_j >= 0 and best_sim >= threshold:
            c = clusters[best_j]
            c["members"].append(i)
            centroid = c["centroid"] * (len(c["members"]) - 1) + emb
            c["centroid"] = centroid / (np.linalg.norm(centroid) + 1e-9)
        else:
            clusters.append({
                "centroid": emb / (np.linalg.norm(emb) + 1e-9),
                "members": [i],
            })
    return [c["members"] for c in clusters]


def _pad_and_crop(img, bbox, pad_ratio: float):
    h, w = img.shape[:2]
    x1, y1, x2, y2 = [int(v) for v in bbox]
    bw, bh = x2 - x1, y2 - y1
    if bw <= 0 or bh <= 0:
        return None
    px = int(bw * pad_ratio)
    py = int(bh * pad_ratio)
    x1 = max(0, x1 - px)
    y1 = max(0, y1 - py)
    x2 = min(w, x2 + px)
    y2 = min(h, y2 + py)
    crop = img[y1:y2, x1:x2]
    return crop if crop.size > 0 else None


def _normalize(xs: list[float]) -> np.ndarray:
    arr = np.asarray(xs, dtype=np.float64)
    lo, hi = float(arr.min()), float(arr.max())
    if hi - lo < 1e-9:
        return np.ones_like(arr)
    return (arr - lo) / (hi - lo)


def find_best_face(
    frames_dir: Path,
    app: FaceAnalysis,
    *,
    padding: float = 0.25,
    min_confidence: float = 0.5,
    identity_threshold: float = 0.35,
) -> FaceResult | None:
    """Run the full session-level pipeline and return the winning crop, or None."""
    frames = list(_iter_frames(frames_dir))
    if not frames:
        return None

    detections: list[dict] = []
    for fp in frames:
        img = cv2.imread(str(fp))
        if img is None:
            continue
        for f in app.get(img):
            if float(f.det_score) < min_confidence:
                continue
            crop = _pad_and_crop(img, f.bbox, 0.0)
            if crop is None:
                continue
            x1, y1, x2, y2 = [int(v) for v in f.bbox]
            detections.append({
                "frame_path": fp,
                "bbox": (x1, y1, x2, y2),
                "embedding": np.asarray(f.normed_embedding, dtype=np.float32),
                "det_score": float(f.det_score),
                "sharpness": _sharpness(crop),
                "size": max(1, (x2 - x1) * (y2 - y1)),
                "frontality": _frontality_from_kps(np.asarray(f.kps)),
            })

    if not detections:
        return None

    clusters = _cluster_embeddings(
        [d["embedding"] for d in detections], identity_threshold
    )
    largest = max(clusters, key=len)
    candidates = [detections[i] for i in largest]

    sharp = _normalize([d["sharpness"] for d in candidates])
    front = _normalize([d["frontality"] for d in candidates])
    size = _normalize([d["size"] for d in candidates])
    score = _normalize([d["det_score"] for d in candidates])
    combined = 0.4 * sharp + 0.3 * front + 0.2 * size + 0.1 * score
    best_idx = int(np.argmax(combined))
    best = candidates[best_idx]
    best_score = float(combined[best_idx])

    img = cv2.imread(str(best["frame_path"]))
    crop = _pad_and_crop(img, best["bbox"], padding)
    if crop is None:
        return None

    return FaceResult(
        crop=crop,
        embedding=best["embedding"],
        score=best_score,
        src_frame=best["frame_path"],
        sharpness=best["sharpness"],
        frontality=best["frontality"],
        size=best["size"],
        det_score=best["det_score"],
    )


def make_face_app(det_size: int = 640) -> FaceAnalysis:
    """First call downloads ~280MB buffalo_l model to ~/.insightface/models/."""
    app = FaceAnalysis(
        name="buffalo_l",
        providers=["CPUExecutionProvider"],
        allowed_modules=["detection", "recognition"],
    )
    app.prepare(ctx_id=-1, det_size=(det_size, det_size))
    return app
