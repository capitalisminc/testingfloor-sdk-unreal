#!/usr/bin/env python3
"""Create a source plugin ZIP from the validated release manifest."""

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from check_release import ROOT, validate


def package(root, output):
    files = validate(root)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation protects an existing release from accidental replacement.
    with ZipFile(output, "x", compression=ZIP_DEFLATED) as archive:
        for file in sorted(files):
            entry = ZipInfo("TestingFloor/" + file.relative_to(root).as_posix())
            entry.date_time = (2026, 1, 1, 0, 0, 0)
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, file.read_bytes())
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(package(ROOT, args.output))
