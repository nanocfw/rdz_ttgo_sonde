#!/usr/bin/env python3
"""Manage the firmware version_id for PU5WDZ OTA builds.

Two subcommands, both driven by `make ota`:

  bump   Rewrite the `version_id` line in RX_FSK/version.h to "pu5wdz<timestamp>"
         (local time, YYYYMMDDHHMMSS) so every build gets a unique id. Run BEFORE
         the compile so the new id is baked into the binary.

  info   Read version.h and print "<version_id>-<Letter><Number>" (Letter = FS_MAJOR
         as A=1..., Number = FS_MINOR) — the string served as update-info.html and
         used by upd.html's compatibility gating.

The version_id prefix is intentionally fixed to "pu5wdz" to identify this fork/server.
"""
import argparse
import re
import sys
from datetime import datetime
from pathlib import Path

VERSION_H = Path(__file__).resolve().parent.parent / "RX_FSK" / "version.h"
PREFIX = "pu5wdz-"

VERSION_ID_RE = re.compile(r'(const\s+char\s*\*\s*version_id\s*=\s*")([^"]*)(";)')
FS_MAJOR_RE = re.compile(r'FS_MAJOR\s*=\s*(\d+)')
FS_MINOR_RE = re.compile(r'FS_MINOR\s*=\s*(\d+)')


def read_version_h():
    try:
        return VERSION_H.read_text()
    except OSError as e:
        sys.exit(f"ota_version: cannot read {VERSION_H}: {e}")


def find(regex, text, what):
    m = regex.search(text)
    if not m:
        sys.exit(f"ota_version: could not find {what} in {VERSION_H}")
    return m


def cmd_bump(_args):
    text = read_version_h()
    find(VERSION_ID_RE, text, "version_id")  # validate presence before writing
    new_id = PREFIX + datetime.now().strftime("%Y%m%d%H%M%S")
    new_text = VERSION_ID_RE.sub(lambda m: m.group(1) + new_id + m.group(3), text, count=1)
    VERSION_H.write_text(new_text)
    print(new_id)


def cmd_info(_args):
    text = read_version_h()
    version_id = find(VERSION_ID_RE, text, "version_id").group(2)
    fs_major = int(find(FS_MAJOR_RE, text, "FS_MAJOR").group(1))
    fs_minor = int(find(FS_MINOR_RE, text, "FS_MINOR").group(1))
    letter = chr(ord("A") + fs_major - 1)
    print(f"{version_id}-{letter}{fs_minor}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("bump", help="rewrite version_id to pu5wdz-<timestamp>").set_defaults(func=cmd_bump)
    sub.add_parser("info", help="print <version_id>-<Letter><Number>").set_defaults(func=cmd_info)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
