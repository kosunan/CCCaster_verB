"""キャラ選択メニューの実入力・D合意・背景ローカル設定を検査する。"""
import re


def verify(texts):
    errors = []
    for side, text in enumerate(texts, 1):
        events = re.findall(r'\[SelectionOptions\] (.*)', text)
        requests = re.findall(r'DELAY request=(\d+) accepted=1', '\n'.join(events))
        expected = ['1', '2'] if side == 1 else ['3']
        if requests != expected:
            errors.append(f'P{side}: D操作が不足または不正: {requests}')
        if events.count('OPEN') != 1 or events.count('CLOSE') != 1:
            errors.append(f'P{side}: メニュー開閉が不正')
        expected_bg = ['OFF', 'ON'] if side == 1 else ['OFF']
        bg = re.findall(r'BACKGROUND animation=(ON|OFF) mode=20', '\n'.join(events))
        # 元からOFFの端末では左OFFは無変更であり、書込みログは出ない。
        # ホストのOFF→ON実変更と両側の戦闘時設定は引き続き必須。
        if bg != expected_bg and not (side == 2 and not bg):
            errors.append(f'P{side}: 背景切替が不足または不正: {bg}')
        hud = re.findall(r'HUD mode=(\w+)', '\n'.join(events))
        expected_hud = ['DETAILED', 'HIDDEN', 'DETAILED'] if side == 1 else ['DETAILED', 'HIDDEN']
        if hud != expected_hud:
            errors.append(f'P{side}: HUD切替が不足または不正: {hud}')
        battle_hud = re.findall(r'BATTLE animation=\d+ delay=\d+ hud=(\w+)', '\n'.join(events))
        if not battle_hud or any(mode != expected_hud[-1] for mode in battle_hud):
            errors.append(f'P{side}: 戦闘開始後のHUDが不正: {battle_hud}')
        battle = re.findall(r'BATTLE animation=(\d+) delay=(\d+)', '\n'.join(events))
        if not battle or any(pair != ('1' if side == 1 else '0', '3') for pair in battle):
            errors.append(f'P{side}: 戦闘開始後の背景設定またはD合意が不正: {battle}')
        close = text.find('[SelectionOptions] CLOSE')
        selected = text.find('[Select] LOCAL ')
        if close < 0 or selected < close:
            errors.append(f'P{side}: メニューを閉じる前にキャラが確定、または未確定')
    return dict(passed=not errors, errors=errors)
