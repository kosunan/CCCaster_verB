import unittest
from analyze_input_send import analyze


class InputSendTests(unittest.TestCase):
    def test_first_success_and_unmatched_are_distinct(self):
        result = analyze('''[InputCaptureSend] f=12 begin=60 end=120 published=180
[InputSend] f=12 dispatch=240 prepared=300 submit=360 complete=420 ok=0 direct=1
[InputSend] f=12 dispatch=300 prepared=360 submit=420 complete=480 ok=1 direct=1
[InputSend] f=12 dispatch=600 prepared=660 submit=720 complete=780 ok=1 direct=1
[InputCaptureSend] f=13 begin=800 end=860 published=900''')
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['unmatched'], [13])
        self.assertEqual(result['failed_send_reports'], 1)
        self.assertEqual(result['metrics']['capture_to_submit']['median'], 6)
        self.assertEqual(result['metrics']['ready_to_submit']['median'], 5)

    def test_bad_chronology_and_dropped_trace_fail(self):
        result = analyze('''[InputCaptureSend] f=12 begin=60 end=120 published=180
[InputSend] f=12 dispatch=70 prepared=80 submit=90 complete=100 ok=1 direct=0
[InputCaptureSendDropped] count=1''')
        self.assertEqual(result['trace_errors'], 2)
        self.assertEqual(result['matched'], 0)


if __name__ == '__main__':
    unittest.main()
