import unittest
from verify_selection_options import verify


class SelectionOptionsTests(unittest.TestCase):
    def logs(self):
        result = []
        for side in (1, 2):
            events = ['OPEN']
            events += [f'DELAY request={d} accepted=1' for d in ([1, 2] if side == 1 else [3])]
            events += [f'BACKGROUND animation={value} mode=20' for value in (['OFF', 'ON'] if side == 1 else ['OFF'])]
            hud = ['DETAILED', 'HIDDEN', 'DETAILED'] if side == 1 else ['DETAILED', 'HIDDEN']
            events += [f'HUD mode={value}' for value in hud]
            events += ['RESOLUTION accepted=1']*4
            events += ['DISPLAY action=fullscreen applied=1 size=960x720 fullscreen=1',
                       'DISPLAY action=fullscreen applied=1 size=960x720 fullscreen=0']
            for name in ('CHARACTER_FILTER', 'SCREEN_FILTER', 'ASPECT_RATIO', 'VIEW_FPS'):
                events += [f'NATIVE option={name} value=1', f'NATIVE option={name} value=0']
            events += ['CLOSE', f'BATTLE animation={1 if side == 1 else 0} delay=3 hud={hud[-1]}']
            text = '\n'.join('[SelectionOptions] ' + event for event in events)
            for size, fullscreen in [('800x600',0),('960x720',0),('800x600',1),('960x720',1)]:
                text += f'\n[NativeResolution] COMPLETE applied=1 requested={size} backbuffer={size} fullscreen={fullscreen} window={size} hr=0x00000000'
            result.append(text + '\n[Select] LOCAL epoch=65536\n')
        return result

    def test_complete(self):
        self.assertTrue(verify(self.logs())['passed'])

    def test_client_already_off(self):
        logs = self.logs()
        logs[1] = logs[1].replace('[SelectionOptions] BACKGROUND animation=OFF mode=20', '')
        self.assertTrue(verify(logs)['passed'])
        logs[1] = logs[1].replace('animation=0 delay=3', 'animation=1 delay=3')
        self.assertFalse(verify(logs)['passed'])

    def test_bad_delay_background_and_input_leak(self):
        for old, new in [('delay=3', 'delay=2'), ('animation=0 delay', 'animation=1 delay'),
                         ('accepted=1', 'accepted=0'), ('CLOSE', 'MISSING'),
                         ('[SelectionOptions] OPEN', '[Select] LOCAL early\n[SelectionOptions] OPEN')]:
            with self.subTest(old=old):
                logs = self.logs()
                logs[1] = logs[1].replace(old, new)
                self.assertFalse(verify(logs)['passed'])

    def test_missing_battle_does_not_pass(self):
        logs = self.logs()
        logs[0] = logs[0].split('[SelectionOptions] BATTLE')[0]
        self.assertFalse(verify(logs)['passed'])

    def test_missing_or_wrong_hud_does_not_pass(self):
        for old, new in [('HUD mode=HIDDEN', 'HUD mode=NORMAL'), ('hud=HIDDEN', 'hud=DETAILED')]:
            logs = self.logs()
            logs[1] = logs[1].replace(old, new)
            self.assertFalse(verify(logs)['passed'])

    def test_display_and_native_failures(self):
        for old, new in [('applied=1', 'applied=0'), ('size=960x720', 'size=800x600'),
                         ('NATIVE option=VIEW_FPS value=1', ''), ('NATIVE option=ASPECT_RATIO value=1', 'NATIVE option=ASPECT_RATIO value=7')]:
            logs = self.logs()
            logs[1] = logs[1].replace(old, new)
            self.assertFalse(verify(logs)['passed'])

    def test_native_reset_is_required(self):
        for old,new in [('RESOLUTION accepted=1','RESOLUTION accepted=0'),
                        ('backbuffer=800x600','backbuffer=1920x1080'),
                        ('window=960x720','window=1920x1080'),
                        ('hr=0x00000000','hr=0x8876086C'),
                        ('[NativeResolution] COMPLETE','[NativeResolution] MISSING')]:
            with self.subTest(old=old):
                logs=self.logs()
                logs[0]=logs[0].replace(old,new)
                self.assertFalse(verify(logs)['passed'])


if __name__ == '__main__':
    unittest.main()
