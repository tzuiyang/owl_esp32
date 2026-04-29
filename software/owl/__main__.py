"""CLI entrypoint: `python -m owl <subcommand>`.

Subcommands
-----------
process   Walk a directory of session folders, write best_face.jpg, transcribe
          audio, and upsert into the people DB.
list      Print one line per person currently in the DB.
show      Print details + every encounter for one person.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .db import (
    get_encounters,
    get_person,
    list_people,
    open_db,
)
from .face import make_face_app
from .process import process_session


def _default_db_path(input_dir: Path | None) -> Path:
    if input_dir is not None:
        return input_dir / "owl.sqlite"
    return Path("data") / "owl.sqlite"


def cmd_process(args: argparse.Namespace) -> int:
    if not args.input_dir.is_dir():
        print(f"error: {args.input_dir} is not a directory", file=sys.stderr)
        return 2

    db_path = args.db_path or _default_db_path(args.input_dir)
    conn = open_db(db_path)
    print(f"db: {db_path}")

    app = make_face_app(det_size=args.det_size)

    processed = 0
    for entry in sorted(args.input_dir.iterdir()):
        if not entry.is_dir():
            continue
        print(f"[session] {entry.name}")
        if process_session(
            entry, app, conn,
            padding=args.padding,
            min_confidence=args.min_confidence,
            identity_threshold=args.identity_threshold,
            person_match_threshold=args.person_match_threshold,
            jpeg_quality=args.jpeg_quality,
            transcribe=not args.no_transcribe,
        ):
            processed += 1

    print(f"done. processed {processed} new session(s).")
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    db_path = args.db_path or _default_db_path(None)
    if not db_path.exists():
        print(f"no db at {db_path}", file=sys.stderr)
        return 1
    conn = open_db(db_path)
    people = list_people(conn)
    if not people:
        print("no people yet.")
        return 0
    for p in people:
        name = p["name"] or "-"
        print(
            f"#{p['id']:<4} {name:<20} "
            f"encounters={p['encounter_count']:<3} "
            f"last={p['last_met']}  "
            f"face={p['face_crop_path']}"
        )
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    db_path = args.db_path or _default_db_path(None)
    if not db_path.exists():
        print(f"no db at {db_path}", file=sys.stderr)
        return 1
    conn = open_db(db_path)
    person = get_person(conn, args.id)
    if person is None:
        print(f"no person with id={args.id}", file=sys.stderr)
        return 1
    name = person["name"] or "(no name)"
    print(f"#{person['id']}  {name}")
    print(f"  face:      {person['face_crop_path']}")
    print(f"  first met: {person['first_met']}")
    print(f"  last met:  {person['last_met']}")
    encounters = get_encounters(conn, args.id)
    print(f"  encounters: {len(encounters)}")
    for enc in encounters:
        print()
        print(f"  --- {enc['started_at']} ({Path(enc['session_dir']).name}) ---")
        if enc["audio_paths"]:
            print("  audio:")
            for audio_path in enc["audio_paths"]:
                print(f"    {audio_path}")
        print(enc["transcript"] or "(no transcript)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="owl")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_proc = sub.add_parser("process", help="Process all sessions in input-dir")
    p_proc.add_argument("--input-dir", type=Path, required=True)
    p_proc.add_argument("--db-path", type=Path, default=None,
                        help="default: <input-dir>/owl.sqlite")
    p_proc.add_argument("--padding", type=float, default=0.25)
    p_proc.add_argument("--min-confidence", type=float, default=0.5)
    p_proc.add_argument("--identity-threshold", type=float, default=0.35,
                        help="intra-session clustering threshold")
    p_proc.add_argument("--person-match-threshold", type=float, default=0.45,
                        help="cross-session identity threshold")
    p_proc.add_argument("--det-size", type=int, default=640)
    p_proc.add_argument("--jpeg-quality", type=int, default=95)
    p_proc.add_argument("--no-transcribe", action="store_true",
                        help="skip Whisper; useful for face-only debugging")
    p_proc.set_defaults(func=cmd_process)

    p_list = sub.add_parser("list", help="List people in db")
    p_list.add_argument("--db-path", type=Path, default=None)
    p_list.set_defaults(func=cmd_list)

    p_show = sub.add_parser("show", help="Show one person + all their encounters")
    p_show.add_argument("id", type=int)
    p_show.add_argument("--db-path", type=Path, default=None)
    p_show.set_defaults(func=cmd_show)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
