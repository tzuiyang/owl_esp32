"""Backwards-compatible face-only entrypoint.

Walks each session, writes one ``best_face.jpg`` per session. Does NOT
transcribe audio or write to the people DB — for the full pipeline use::

    python -m owl process --input-dir ./data
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import cv2

from owl.face import find_best_face, make_face_app


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input-dir", type=Path, required=True)
    ap.add_argument(
        "--output-dir", type=Path, default=None,
        help="If set, write <session>.jpg here. Otherwise write "
             "best_face.jpg inside each session.",
    )
    ap.add_argument("--padding", type=float, default=0.25)
    ap.add_argument("--min-confidence", type=float, default=0.5)
    ap.add_argument("--identity-threshold", type=float, default=0.35)
    ap.add_argument("--det-size", type=int, default=640)
    ap.add_argument("--jpeg-quality", type=int, default=95)
    args = ap.parse_args()

    if not args.input_dir.is_dir():
        print(f"error: {args.input_dir} is not a directory", file=sys.stderr)
        return 2

    app = make_face_app(det_size=args.det_size)

    saved = 0
    for entry in sorted(args.input_dir.iterdir()):
        if not entry.is_dir():
            continue
        frames_dir = entry / "image"
        if not frames_dir.is_dir():
            print(f"[skip] {entry.name}: no image/ subfolder")
            continue
        print(f"[session] {entry.name}")
        result = find_best_face(
            frames_dir, app,
            padding=args.padding,
            min_confidence=args.min_confidence,
            identity_threshold=args.identity_threshold,
        )
        if result is None:
            print("  [skip] no face")
            continue
        if args.output_dir is not None:
            out_path = args.output_dir / f"{entry.name}.jpg"
            out_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            out_path = entry / "best_face.jpg"
        cv2.imwrite(str(out_path), result.crop,
                    [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_quality])
        print(
            f"  best: {result.src_frame.name} "
            f"(sharp={result.sharpness:.1f} front={result.frontality:.2f} "
            f"size={result.size} det={result.det_score:.2f}) -> {out_path}"
        )
        saved += 1

    print(f"done. sessions with a saved face: {saved}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
