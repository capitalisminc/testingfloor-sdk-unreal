#!/usr/bin/env python3
"""Validate the files included in a public SDK source release."""

import hashlib
import json
from pathlib import Path, PurePosixPath
import re


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = "release-files.txt"
VENDOR = "Source/TestingFloorSdk/Private/ThirdParty/QrCodeGenerator/"
VENDOR_HASHES = {
    "qrcodegen.c": "300eff07ee25baaa7578f20284411638154716379437391e7e689c0e6ce81403",
    "qrcodegen.h": "e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589",
}
SECRET_PATTERNS = (
    re.compile(r"-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----"),
    re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{50,}\b"),
    re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b"),
    re.compile(r"https?://[^\s/:]+:[^\s/@]+@"),
)


def release_files(root=ROOT):
    """Resolve an explicit manifest without accepting symlinks or path traversal."""
    entries = (root / MANIFEST).read_text(encoding="utf-8").splitlines()
    if not entries or len(entries) != len(set(entries)):
        raise ValueError("Release manifest must be nonempty and contain no duplicates")
    files = []
    for entry in entries:
        path = PurePosixPath(entry)
        if not entry or path.is_absolute() or ".." in path.parts or "\\" in entry:
            raise ValueError(f"Invalid release path: {entry}")
        if any(part.startswith(".") for part in path.parts):
            raise ValueError(f"Hidden files cannot be distributed: {entry}")
        if path.parts[0] != "Source" and entry not in {
            "TestingFloor.uplugin", "README.md", "LICENSE.md", "CHANGELOG.md", "ThirdPartyNotices.md", "CONTRIBUTING.md", "AGENTS.md", "SDK_CONTRACT.md"
        }:
            raise ValueError(f"Unexpected release file: {entry}")
        if path.parts[0] == "Source" and path.suffix not in {".cpp", ".h", ".c", ".cs", ".md"}:
            raise ValueError(f"Unexpected source extension: {entry}")
        file = root / entry
        if not file.is_file() or file.is_symlink():
            raise ValueError(f"Missing or symlinked release file: {entry}")
        # Check absolute ancestors too, so a symlinked Source directory cannot escape the root.
        if any((root / parent).is_symlink() for parent in path.parents):
            raise ValueError(f"Symlinked release directory: {entry}")
        files.append(file)
    return files


def validate(root=ROOT):
    files = release_files(root)
    required = {"TestingFloor.uplugin", "README.md", "LICENSE.md", "CHANGELOG.md", "ThirdPartyNotices.md", "CONTRIBUTING.md", "AGENTS.md", "SDK_CONTRACT.md"}
    included = {file.relative_to(root).as_posix() for file in files}
    if not required.issubset(included):
        raise ValueError("Release manifest omits required metadata or licenses")
    source_files = {p.relative_to(root).as_posix() for p in (root / "Source").rglob("*") if p.is_file()}
    if source_files != {entry for entry in included if entry.startswith("Source/")}:
        raise ValueError("Source tree and release manifest disagree")
    descriptor = json.loads((root / "TestingFloor.uplugin").read_text(encoding="utf-8"))
    expected_modules = {"TestingFloorSdk": "Runtime", "TestingFloorSdkTests": "Editor"}
    modules = descriptor["Modules"]
    if len(modules) != len(expected_modules) or {m["Name"]: m["Type"] for m in modules} != expected_modules:
        raise ValueError("Unexpected plugin module or module type")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", descriptor["VersionName"]) or descriptor["CreatedBy"] != "Testing Floor":
        raise ValueError("Check the plugin's public version and publisher metadata")
    if descriptor["CanContainContent"]:
        raise ValueError("This SDK release must be content-free")
    for module in expected_modules:
        if f"Source/{module}/{module}.Build.cs" not in included:
            raise ValueError(f"Missing module rules: {module}")
    for name, expected in VENDOR_HASHES.items():
        if VENDOR + name not in included:
            raise ValueError(f"Missing vendored source: {name}")
        if hashlib.sha256((root / VENDOR / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Vendored source differs from its licensed upstream: {name}")
    for file in files:
        text = file.read_text(encoding="utf-8")
        if any(pattern.search(text) for pattern in SECRET_PATTERNS):
            raise ValueError(f"Possible credential in {file.relative_to(root)}; review before release")
    if "MIT License" not in (root / "LICENSE.md").read_text(encoding="utf-8"):
        raise ValueError("Missing MIT license")
    return files


if __name__ == "__main__":
    try:
        files = validate()
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(str(error)) from error
    print(f"Public source release checked: {len(files)} allowlisted files")
