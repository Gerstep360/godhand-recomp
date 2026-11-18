"""Safety checks for importing VU captures; no disc or native compiler required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("godhand_build", Path(__file__).resolve().parents[2] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class VuBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, value in (("ROOT", self.root), ("BIN", self.root / "bin"),
                            ("BUILD", self.root / "build" / "port"), ("GENERATED", self.root / "generated")):
            p = patch.object(build, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.running = patch.object(build, "game_running", return_value=False)
        self.running.start()
        self.addCleanup(self.running.stop)
        self.capture = self.root / "bin" / "logs" / "vu1_missing"
        self.capture.mkdir(parents=True)

    def capture_zero_image(self):
        # FNV-1a of 16 KiB of zero bytes (the name is checked before invoking the tool).
        image = self.capture / "vu1_9c1bda7f8c872325.bin"
        image.write_bytes(bytes(16384))
        return image

    def test_no_captures_does_not_touch_existing_programs(self):
        output = build.GENERATED / "vu1"
        output.mkdir(parents=True)
        marker = output / "vu1_programs.cpp"
        marker.write_text("keep this")
        self.assertEqual(build.vu1(), 1)
        self.assertEqual(marker.read_text(), "keep this")

    def test_bad_capture_rejected_before_configure(self):
        for data in (b"short", bytes(16384)):
            with self.subTest(size=len(data)), patch.object(build, "configure") as configure:
                (self.capture / "vu1_0000000000000000.bin").write_bytes(data)
                self.assertEqual(build.vu1(), 1)
                configure.assert_not_called()

    def test_missing_explicit_directory_is_an_error(self):
        self.assertEqual(build.vu1([str(self.root / "absent")]), 1)

    def test_incomplete_corpus_keeps_existing_generated_files(self):
        image = self.capture_zero_image()
        output = build.GENERATED / "vu1"
        output.mkdir(parents=True)
        registry = output / "vu1_programs.cpp"
        registry.write_text("registerVu1CompiledProgram(0x1234ull, fn);")

        def translate(args):
            staging = args[2]
            staging.mkdir(parents=True)
            (staging / "vu1_programs.cpp").write_text("registerVu1CompiledProgram(0x9C1BDA7F8C872325ull, fn);")
            return 0

        with patch.object(build, "configure", return_value=0), patch.object(build, "build", return_value=0), \
                patch.object(build, "run", side_effect=translate):
            self.assertEqual(build.vu1(), 1)
        self.assertEqual(registry.read_text(), "registerVu1CompiledProgram(0x1234ull, fn);")
        self.assertEqual((build.ROOT / "build" / "vu_corpus" / image.name).read_bytes(), image.read_bytes())

    def test_repeat_import_keeps_source_timestamps(self):
        self.capture_zero_image()

        def translate(args):
            staging = args[2]
            staging.mkdir(parents=True, exist_ok=True)
            (staging / "vu1_programs.cpp").write_text("registerVu1CompiledProgram(0x9C1BDA7F8C872325ull, fn);")
            return 0

        with patch.object(build, "configure", return_value=0), patch.object(build, "build", return_value=0), \
                patch.object(build, "run", side_effect=translate):
            self.assertEqual(build.vu1(), 0)
            output = build.GENERATED / "vu1" / "vu1_programs.cpp"
            original_mtime = output.stat().st_mtime_ns
            # Emulate a later import after the original log folder has been moved or cleaned.
            self.capture_zero_image().unlink()
            self.assertEqual(build.vu1(), 0)
            self.assertEqual(output.stat().st_mtime_ns, original_mtime)


if __name__ == "__main__":
    unittest.main()
