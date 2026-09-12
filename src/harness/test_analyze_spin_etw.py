"""実ETL無しでETW突合の区間計算・誤帰属防止を検証する。"""
import unittest
import tempfile
from pathlib import Path
from analyze_spin_etw import Trace, union_length, read_spin

class SoundWindows(unittest.TestCase):
    def test_input_wait_reason_and_absolute_window(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"game.log"
            path.write_text("[InputWait] f=50 reason=1 begin=100 end=60100 pid=42 tid=7\n[InputWait] f=50 reason=2 begin=60200 end=120200 pid=42 tid=7\n",encoding="utf-8")
            rows=read_spin(path)
            self.assertEqual([r['name'] for r in rows],['remoteInputWait','metronomeInputWait'])
            self.assertEqual(rows[0]['end']-rows[0]['begin'],60000)

    def test_replay_restore_and_execution_are_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"game.log"
            path.write_text("[ReplayWork] f=40 from=36 begin=100 restoreEnd=700 end=2500 saves=60 prepare=120 pid=42 tid=7\n", encoding="utf-8")
            rows = read_spin(path)
            self.assertEqual([(r["name"], r["begin"], r["end"]) for r in rows],
                             [("replayRestore", 100, 700), ("replayExecution", 700, 2500)])

    def test_same_frame_replay_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.log"
            path.write_text(
                "[SoundProbe] f=20 replay=0 calls=1 suppressed=0 ticks=600 begin=100 end=700 pid=42 tid=7\n"
                "[SoundProbe] f=20 replay=1 calls=1 suppressed=1 ticks=600 begin=800 end=1400 pid=42 tid=7\n",
                encoding="utf-8")
            rows = read_spin(path)
            self.assertEqual([r["name"] for r in rows], ["soundNormal", "soundReplay"])
            self.assertEqual([r["begin"] for r in rows], [100, 800])


def switch(time, cpu, old, new):
    return dict(type="cswitch", qpc=time, cpu=cpu, old_tid=old, new_tid=new)


def interrupt(kind, begin, end, cpu=0, routine=0x1010):
    return dict(type=kind, begin_qpc=begin, end_qpc=end, cpu=cpu, routine=routine)


def trace(*records, **meta):
    defaults = dict(type="meta", qpc_hz=60_000_000, clock="qpc", complete=True, final=True,
                    lost_events=0, lost_buffers=0)
    defaults.update(meta)
    return Trace([defaults, *records])


def window(begin=100, end=400):
    return dict(begin=begin, end=end, tid=7, pid=42, qpc_hz=60_000_000)


class SpinEtwTests(unittest.TestCase):
    def test_union_nested_and_boundary(self):
        self.assertEqual(union_length([(10, 30), (15, 20), (30, 40), (80, 90)]), 40)

    def test_cpu_specific_and_nested_interrupt_union(self):
        data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0),
                     interrupt("dpc", 150, 250), interrupt("isr", 180, 220),
                     interrupt("dpc", 100, 400, cpu=1))
        result = data.attribute(window())
        self.assertEqual(result["status"], "observed")
        self.assertEqual(result["interrupt_us"], 100 / 60)
        self.assertEqual(result["on_cpu_other_us"], 200 / 60)
        self.assertEqual(len(result["interrupts"]), 2)

    def test_migration_and_off_cpu(self):
        data = trace(switch(0, 0, 0, 7), switch(200, 0, 7, 9),
                     switch(300, 1, 2, 7), switch(500, 1, 7, 2),
                     interrupt("dpc", 250, 390, cpu=0),
                     interrupt("isr", 320, 350, cpu=1))
        result = data.attribute(window())
        self.assertEqual(result["off_cpu_us"], 100 / 60)
        self.assertEqual(result["interrupt_us"], 30 / 60)
        self.assertEqual(result["on_cpu_other_us"], 170 / 60)

    def test_missing_bracketing_switch_is_unknown(self):
        data = trace(switch(200, 0, 0, 7), switch(500, 0, 7, 0))
        result = data.attribute(window())
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["scheduler_unknown_us"], 100 / 60)

    def test_loss_prevents_conclusion(self):
        data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0), lost_events=3)
        self.assertEqual(data.attribute(window())["status"], "unknown")

    def test_missing_final_or_loss_counters_prevents_conclusion(self):
        for fields in ({"final": False}, {"complete": False}, {"lost_buffers": None}, {"lost_events": None}):
            with self.subTest(fields=fields):
                data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0), **fields)
                self.assertEqual(data.attribute(window())["status"], "unknown")

    def test_half_open_boundaries(self):
        data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0),
                     interrupt("isr", 50, 100), interrupt("dpc", 400, 450))
        self.assertEqual(data.attribute(window())["interrupt_us"], 0)

    def test_kernel_module_load_and_unload(self):
        image = dict(type="image", base=0x1000, size=0x100, pid=4, path="driver.sys")
        data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0),
                     dict(image, qpc=20, event="Load"), dict(image, qpc=300, event="Unload"),
                     interrupt("isr", 150, 200), interrupt("isr", 350, 380))
        result = data.attribute(window())
        self.assertEqual(result["drivers_us"]["driver.sys"], 50 / 60)
        self.assertEqual(result["drivers_us"]["unknown"], 30 / 60)

    def test_tid_reuse_is_unknown(self):
        data = trace(switch(0, 0, 0, 7), switch(500, 0, 7, 0),
                     dict(type="thread", qpc=0, tid=7, pid=42, event="Start"),
                     dict(type="thread", qpc=1000, tid=7, pid=99, event="Start"))
        self.assertIn("thread_id_reused", data.attribute(window())["reasons"])

    def test_past_tid_reuse_with_bracketed_lifetime_is_known(self):
        data = trace(switch(50, 0, 0, 7), switch(500, 0, 7, 0),
                     dict(type="thread", qpc=0, tid=7, pid=99, event="Start"),
                     dict(type="thread", qpc=10, tid=7, pid=99, event="Stop"),
                     dict(type="thread", qpc=20, tid=7, pid=42, event="Start"),
                     dict(type="thread", qpc=550, tid=7, pid=42, event="Stop"))
        self.assertEqual(data.attribute(window())["status"], "observed")

    def test_reused_tid_interval_must_stay_inside_lifetime(self):
        data = trace(switch(0, 0, 0, 7), switch(600, 0, 7, 0),
                     dict(type="thread", qpc=0, tid=7, pid=99, event="Start"),
                     dict(type="thread", qpc=10, tid=7, pid=99, event="Stop"),
                     dict(type="thread", qpc=150, tid=7, pid=42, event="Start"),
                     dict(type="thread", qpc=350, tid=7, pid=42, event="Stop"))
        self.assertIn("thread_id_reused", data.attribute(window())["reasons"])
        self.assertEqual(data.attribute(window(150, 350))["status"], "observed")

    def test_qpc_conversion_matches_platform(self):
        data = Trace([dict(type="meta", qpc_hz=10_000_000, clock="qpc", complete=True, lost_events=0)])
        self.assertEqual(data.tick(123456789), 740740734)


if __name__ == "__main__":
    unittest.main()
