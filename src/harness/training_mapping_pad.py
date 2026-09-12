"""手操作UI検証用の仮想DS4。試験用入力バイパスを使わず通常のINI経路を通す。

既存のViGEm環境だけを利用。標準入力: square/cross/circle/triangle/east/west/up/neutral/quit。
デバイスだけを作成し、ゲーム起動・ユーザー設定の変更・既存プロセス停止は行わない。
"""
import time
import vgamepad as vg


def main():
    pad = vg.VDS4Gamepad()
    print('Virtual DS4 ready (normal DirectInput / no CCCASTER_TEST_VIRTUAL_PRODUCT)', flush=True)
    buttons = dict(square=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE, cross=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                   circle=vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE, triangle=vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE)
    directions = dict(east=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
                      west=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,
                      up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH)
    try:
        for line in iter(input, 'quit'):
            fields = line.split()
            if not fields:
                continue
            command = fields[0]
            seconds = min(5, max(.05, float(fields[1]))) if len(fields) > 1 else .15
            pad.reset()
            if command in buttons:
                pad.press_button(buttons[command])
            elif command in directions:
                pad.directional_pad(directions[command])
            pad.update()
            time.sleep(seconds)
            pad.reset(); pad.update()
            print('Completed:', line, flush=True)
    finally:
        pad.reset(); pad.update()


if __name__ == '__main__':
    main()
