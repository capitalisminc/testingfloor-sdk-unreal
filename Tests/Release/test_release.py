from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import check_release
import package


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "sdk"
        shutil.copytree(ROOT, self.root, ignore=shutil.ignore_patterns(".git", "__pycache__", "dist"))

    def test_package_contains_only_manifest_and_has_no_history_or_local_config(self):
        (self.root / ".env").write_text("LOCAL_ONLY=example")
        (self.root / ".git").mkdir()
        (self.root / ".git" / "config").write_text("local metadata")
        (self.root / "Binaries").mkdir(exist_ok=True)
        (self.root / "Binaries" / "example.bin").write_bytes(b"generated")
        output = Path(self.temp.name) / "sdk.zip"
        package.package(self.root, output)
        with ZipFile(output) as archive:
            expected = {"TestingFloor/" + p.relative_to(self.root).as_posix() for p in check_release.validate(self.root)}
            self.assertEqual(set(archive.namelist()), expected)
            self.assertEqual(archive.read("TestingFloor/TestingFloor.uplugin"), (self.root / "TestingFloor.uplugin").read_bytes())
        with self.assertRaises(FileExistsError):
            package.package(self.root, output)

    def test_path_traversal_rejected(self):
        with (self.root / check_release.MANIFEST).open("a") as manifest:
            manifest.write("../private.txt\n")
        with self.assertRaisesRegex(ValueError, "Invalid release path"):
            check_release.validate(self.root)

    def test_symlinked_source_rejected(self):
        source = self.root / "Source"
        source.rename(self.root / "other-source")
        source.symlink_to(self.root / "other-source", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Symlinked release directory"):
            check_release.validate(self.root)

    def test_unlisted_source_rejected(self):
        (self.root / "Source" / "unexpected.cpp").write_text("// unexpected source\n")
        with self.assertRaisesRegex(ValueError, "Source tree and release manifest disagree"):
            check_release.validate(self.root)

    def test_private_key_in_allowlisted_file_rejected(self):
        with (self.root / "README.md").open("a") as file:
            file.write("-----BEGIN " + "PRIVATE KEY-----\nexample\n")
        with self.assertRaisesRegex(ValueError, "Possible credential in README.md"):
            check_release.validate(self.root)

    def test_vendor_modification_rejected(self):
        with (self.root / check_release.VENDOR / "qrcodegen.c").open("a") as file:
            file.write("\n// changed\n")
        with self.assertRaisesRegex(ValueError, "Vendored source differs"):
            check_release.validate(self.root)

    def test_missing_license_rejected(self):
        (self.root / "LICENSE.md").unlink()
        with self.assertRaisesRegex(ValueError, "Missing or symlinked release file"):
            check_release.validate(self.root)


if __name__ == "__main__":
    unittest.main()
