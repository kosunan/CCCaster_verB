import tempfile
import unittest
from pathlib import Path
from analyze_sound_api import analyze
from analyze_spin_etw import read_spin

class SoundApiAnalysis(unittest.TestCase):
    def test_nested_wait_is_not_double_counted(self):
        text = """[CombatStress] f=10 mode=2
[SoundApi] seq=1 f=10 replay=0 sound=1 api=WaitForMultipleObjects depth=1 caller=5 buffer=8 result=0 a=1 b=0 c=4294967295 status=0 begin=300 end=600 pid=42 tid=7
[SoundApi] seq=1 f=10 replay=0 sound=1 api=Play depth=0 caller=4 buffer=3 result=0 a=0 b=0 c=0 status=0 begin=200 end=800 pid=42 tid=7
[SoundProbe] seq=1 f=10 replay=0 calls=1 suppressed=0 ticks=900 begin=100 end=1000 pid=42 tid=7 warmup=0
[SoundApi] seq=2 f=10 replay=1 sound=1 api=Play depth=0 caller=4 buffer=3 result=0 a=0 b=0 c=0 status=0 begin=1100 end=1700 pid=42 tid=7
[SoundProbe] seq=2 f=10 replay=1 calls=1 suppressed=0 ticks=900 begin=1000 end=1900 pid=42 tid=7 warmup=0
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.log"
            path.write_text(text, encoding="utf-8")
            result = analyze(path)
            top = result["summary"]["normal"]["top_updates"][0]
            self.assertEqual(top["api_union_us"], 10)
            self.assertEqual(top["residual_us"], 5)
            self.assertEqual(result["summary"]["replay"]["repeat_play_us"]["count"], 1)
            self.assertEqual(result["summary"]["normal"]["first_play_us"]["count"], 1)
            self.assertFalse(result["errors"])
            self.assertEqual(len([r for r in read_spin(path) if r["name"] == "soundApi_Play"]), 2)

if __name__ == "__main__":
    unittest.main()
