#!/usr/bin/env python3
"""Copy the published Testing Floor SDK contract into Tests/Contract.

Tests/Contract holds an unedited copy of
https://github.com/capitalisminc/testingfloor-sdk-contract, and
Tests/contract-source.json records the commit it came from.

    python3 scripts/update_contract.py             # copy the latest published version
    python3 scripts/update_contract.py --check     # confirm the copy matches its recorded commit
    python3 scripts/update_contract.py --from DIR  # try an unpublished copy; records no commit
"""

import argparse
import filecmp
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPOSITORY = "https://github.com/capitalisminc/testingfloor-sdk-contract"
ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "Tests" / "Contract"
SOURCE = ROOT / "Tests" / "contract-source.json"


def files(folder):
    return sorted(
        path.relative_to(folder)
        for path in folder.rglob("*")
        if path.is_file() and ".git" not in path.relative_to(folder).parts
    )


def differences(published, copy):
    expected, actual = files(published), files(copy)
    return [
        str(path)
        for path in sorted(set(expected) | set(actual))
        if path not in expected or path not in actual or not filecmp.cmp(published / path, copy / path, shallow=False)
    ]


def clone(commit, into):
    subprocess.run(["git", "clone", "--quiet", REPOSITORY + ".git", str(into)], check=True)
    if commit:
        subprocess.run(["git", "-C", str(into), "checkout", "--quiet", commit], check=True)
    return subprocess.run(
        ["git", "-C", str(into), "rev-parse", "HEAD"], check=True, capture_output=True, text=True
    ).stdout.strip()


def replace(published):
    shutil.rmtree(TARGET, ignore_errors=True)
    for path in files(published):
        (TARGET / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(published / path, TARGET / path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare the copy with its recorded commit")
    parser.add_argument("--from", dest="local", type=pathlib.Path, help="copy this folder instead of the published one")
    arguments = parser.parse_args()

    if arguments.local:
        replace(arguments.local.resolve())
        print(f"Copied {arguments.local}. Tests/contract-source.json still names the last published copy.")
        return 0

    with tempfile.TemporaryDirectory() as temporary:
        published = pathlib.Path(temporary) / "contract"
        if arguments.check:
            commit = json.loads(SOURCE.read_text())["commit"]
            clone(commit, published)
            changed = differences(published, TARGET)
            if changed:
                print(f"Tests/Contract differs from {REPOSITORY} at {commit}: {', '.join(changed)}", file=sys.stderr)
                print("Run python3 scripts/update_contract.py to refresh it.", file=sys.stderr)
                return 1
            print(f"Tests/Contract matches {REPOSITORY} at {commit}")
            return 0

        commit = clone(None, published)
        replace(published)
        SOURCE.write_text(json.dumps({"repository": REPOSITORY, "commit": commit}, indent=2) + "\n")
        print(f"Copied {REPOSITORY} at {commit}. Run the TestingFloor.Sdk automation tests next.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
