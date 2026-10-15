import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import transcribe as t  # noqa: E402


class Regions(unittest.TestCase):
    def test_threshold_is_adaptive_and_bounded(self):
        quiet_room = [-51.0] * 90 + [-32.0] * 10
        self.assertAlmostEqual(t.noise_adaptive_threshold(quiet_room), -43.0)
        self.assertEqual(t.noise_adaptive_threshold([-120.0] * 50), -65.0)   # lower bound
        self.assertEqual(t.noise_adaptive_threshold([-20.0] * 50), -35.0)    # upper bound

    def test_regions_merge_pad_and_drop_blips(self):
        levels = [-60.0] * 10 + [-30.0] * 5 + [-60.0] * 3 + [-30.0] * 5 + [-60.0] * 20 + [-30.0] * 1 + [-60.0] * 10
        regions = t.active_regions(levels, -45.0, pad_s=0.0)
        self.assertEqual(len(regions), 1)               # gap of 0.3 s merged, 0.1 s blip dropped
        self.assertAlmostEqual(regions[0][0], 1.0)
        self.assertAlmostEqual(regions[0][1], 2.3)

    def test_padding_stays_inside_the_file(self):
        regions = t.active_regions([-30.0] * 10, -45.0, pad_s=0.3)
        self.assertEqual(regions, [(0.0, 1.0)])


class Filtering(unittest.TestCase):
    def test_hallucination_in_silence_is_dropped(self):
        regions = [(10.0, 14.0)]
        segs = [t.Segment(10.5, 13.0, "real speech", "me"), t.Segment(40.0, 42.0, "Thanks for watching", "me")]
        kept = t.keep_voiced(segs, regions)
        self.assertEqual([s.text for s in kept], ["real speech"])

    def test_non_speech_markers_are_ignored(self):
        data = {"transcription": [{"offsets": {"from": 0, "to": 1000}, "text": " [BLANK_AUDIO]"},
                                  {"offsets": {"from": 1000, "to": 2500}, "text": " Привет."}],
                "result": {"language": "ru"}}
        segs, lang = t.parse_whisper_cpp_json(data, "me")
        self.assertEqual(lang, "ru")
        self.assertEqual([(s.start, s.end, s.text) for s in segs], [(1.0, 2.5, "Привет.")])

    def test_known_hallucination_dropped_only_when_short(self):
        self.assertTrue(t.is_hallucination(" Bye.", 0.8))
        self.assertTrue(t.is_hallucination("Thank you!", 1.5))
        self.assertFalse(t.is_hallucination("Thank you", 6.0))           # long segment: keep
        self.assertFalse(t.is_hallucination("Thank you for the offer.", 1.0))  # real sentence: keep

    def test_punctuation_only_text_is_ignored(self):
        data = {"transcription": [{"offsets": {"from": 0, "to": 500}, "text": " ."},
                                  {"offsets": {"from": 500, "to": 900}, "text": " ... "}]}
        self.assertEqual(t.parse_whisper_cpp_json(data, "me")[0], [])


class Clipping(unittest.TestCase):
    def test_segment_start_is_trimmed_to_the_first_speech(self):
        regions = [(9.0, 12.0), (14.0, 15.0)]
        segs = [t.Segment(0.0, 15.0, "long", "me")]          # whisper.cpp started it at the window edge
        out = t.clip_to_regions(segs, regions)
        self.assertEqual((out[0].start, out[0].end), (9.0, 15.0))

    def test_segment_without_speech_is_removed(self):
        self.assertEqual(t.clip_to_regions([t.Segment(30.0, 32.0, "x", "me")], [(9.0, 12.0)]), [])


class Chunks(unittest.TestCase):
    def test_close_regions_are_joined_far_ones_are_not(self):
        levels = [-60.0] * 600
        chunks = t.build_chunks([(1.0, 3.0), (4.0, 6.0), (30.0, 32.0)], levels)
        self.assertEqual(chunks, [(1.0, 6.0), (30.0, 32.0)])

    def test_long_region_is_cut_at_the_quietest_frame(self):
        levels = [-30.0] * 700
        levels[int(25.0 / t.FRAME_S)] = -70.0            # a pause at 25 s, inside the last 6 s window
        chunks = t.build_chunks([(0.0, 40.0)], levels, max_s=28.0)
        self.assertEqual(len(chunks), 2)
        self.assertAlmostEqual(chunks[0][1], 25.0, places=1)
        self.assertAlmostEqual(chunks[1][0], 25.0, places=1)
        self.assertEqual(chunks[1][1], 40.0)

    def test_blips_shorter_than_min_are_dropped(self):
        self.assertEqual(t.build_chunks([(5.0, 5.2)], [-60.0] * 100), [])


class Merging(unittest.TestCase):
    def test_same_speaker_joined_other_speaker_interleaved(self):
        segs = [t.Segment(0, 2, "a", "others"), t.Segment(2.5, 4, "b", "others"),
                t.Segment(3, 5, "c", "me"), t.Segment(20, 22, "d", "others")]
        turns = t.merge_turns(segs)
        self.assertEqual([(x.speaker, x.text) for x in turns],
                         [("others", "a b"), ("me", "c"), ("others", "d")])

    def test_markdown(self):
        turns = [t.Segment(9, 12, "Привет", "me"), t.Segment(3700, 3705, "поздно", "others")]
        md = t.render_markdown(turns, "R0004", "2026-10-15", 3800, "ru", {"me": "Я", "others": "Остальные"})
        self.assertIn("**[00:09] Я:** Привет", md)
        self.assertIn("**[1:01:40] Остальные:** поздно", md)
        self.assertTrue(md.startswith("---\ntitle: Запись R0004"))

    def test_time_format(self):
        self.assertEqual(t.fmt_time(65), "01:05")
        self.assertEqual(t.fmt_time(3661), "1:01:01")


class Sessions(unittest.TestCase):
    def test_pair_from_name_and_wav_path(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            for n in ("R0004ME.WAV", "R0004OT.WAV"):
                (d / n).write_bytes(b"x" * 100)
            me, ot, name = t.find_pair("R0004", d)
            self.assertEqual((me.name, ot.name, name), ("R0004ME.WAV", "R0004OT.WAV", "R0004"))
            me, ot, name = t.find_pair(str(d / "R0004OT.WAV"), None)
            self.assertEqual((me.name, ot.name, name), ("R0004ME.WAV", "R0004OT.WAV", "R0004"))
            self.assertEqual(t.find_pair(None, d)[2], "R0004")


if __name__ == "__main__":
    unittest.main()
