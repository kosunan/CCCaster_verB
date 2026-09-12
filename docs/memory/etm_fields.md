# ETM構造体フィールド索引

[調査資料と注意点](README.md)へ戻る。

参照コミット: `038887d7d8e6e70963ce9eb5780a25ac35cf1cac`。32bit・pack(1)として算出。実ゲーム未確認。
ActorDataの例示VAはP1のsubObj基点、PlayerDataはP1本体、PlayerAuxDataはP1側補助構造体。
PlayerDataの先頭0x33CはEffectData継承部分（exists 4B + ActorData）。未命名の空白は省略。
型は参照元の宣言であり、変数名だけでは意味・符号・有効条件を保証しない。

算術検査: 9構造体のサイズとCHECKOFFSET 19件一致。

## CameraBoxData（0x10 bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `x1` | `int[1]` | `0x0` | 4 | 動的／親構造体基点 | [L222](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L222) |
| `y1` | `int[1]` | `0x4` | 4 | 動的／親構造体基点 | [L223](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L223) |
| `x2` | `int[1]` | `0x8` | 4 | 動的／親構造体基点 | [L224](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L224) |
| `y2` | `int[1]` | `0xC` | 4 | 動的／親構造体基点 | [L225](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L225) |

## RawBoxData（0x8 bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `x1` | `short[1]` | `0x0` | 2 | 動的／親構造体基点 | [L231](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L231) |
| `y1` | `short[1]` | `0x2` | 2 | 動的／親構造体基点 | [L232](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L232) |
| `x2` | `short[1]` | `0x4` | 2 | 動的／親構造体基点 | [L233](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L233) |
| `y2` | `short[1]` | `0x6` | 2 | 動的／親構造体基点 | [L234](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L234) |

## ActorData（0x338 bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `index` | `BYTE[1]` | `0x0` | 1 | 0x00555134 | [L447](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L447) |
| `charID` | `BYTE[1]` | `0x1` | 1 | 0x00555135 | [L448](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L448) |
| `charIDCopy` | `BYTE[1]` | `0x2` | 1 | 0x00555136 | [L449](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L449) |
| `doTrainingAction` | `BYTE[1]` | `0x3` | 1 | 0x00555137 | [L450](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L450) |
| `source` | `BYTE[1]` | `0x4` | 1 | 0x00555138 | [L451](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L451) |
| `someFlag` | `BYTE[1]` | `0x5` | 1 | 0x00555139 | [L452](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L452) |
| `palette` | `BYTE[1]` | `0x6` | 1 | 0x0055513A | [L453](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L453) |
| `moon` | `WORD[1]` | `0x8` | 2 | 0x0055513C | [L455](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L455) |
| `pattern` | `DWORD[1]` | `0xC` | 4 | 0x00555140 | [L457](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L457) |
| `state` | `DWORD[1]` | `0x10` | 4 | 0x00555144 | [L458](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L458) |
| `nextSprite` | `DWORD[1]` | `0x14` | 4 | 0x00555148 | [L459](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L459) |
| `framesInCurrentState` | `DWORD[1]` | `0x18` | 4 | 0x0055514C | [L460](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L460) |
| `isFirstFramePlusOne` | `BYTE[1]` | `0x1C` | 1 | 0x00555150 | [L461](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L461) |
| `didPatternTransition` | `BYTE[1]` | `0x1F` | 1 | 0x00555153 | [L463](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L463) |
| `remainingLoops` | `BYTE[1]` | `0x21` | 1 | 0x00555155 | [L465](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L465) |
| `EFTP1flagset1` | `WORD[1]` | `0x22` | 2 | 0x00555156 | [L466](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L466) |
| `EFTP1flagset2` | `WORD[1]` | `0x24` | 2 | 0x00555158 | [L467](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L467) |
| `commandSpecialVar` | `WORD[1]` | `0x26` | 2 | 0x0055515A | [L468](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L468) |
| `randomAirTech` | `DWORD[1]` | `0x4C` | 4 | 0x00555180 | [L470](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L470) |
| `randomGroundTech` | `DWORD[1]` | `0x50` | 4 | 0x00555184 | [L471](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L471) |
| `notInCombo` | `DWORD[1]` | `0x60` | 4 | 0x00555194 | [L473](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L473) |
| `numFrameAndPatternTransitions` | `DWORD[1]` | `0x64` | 4 | 0x00555198 | [L474](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L474) |
| `defensiveStateFlag` | `DWORD[1]` | `0x68` | 4 | 0x0055519C | [L475](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L475) |
| `defensiveStateQueue` | `DWORD[1]` | `0x84` | 4 | 0x005551B8 | [L477](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L477) |
| `flags` | `DWORD[6]` | `0x88` | 24 | 0x005551BC | [L478](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L478) |
| `exGuard` | `DWORD[1]` | `0xAC` | 4 | 0x005551E0 | [L480](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L480) |
| `exGuardTimer` | `DWORD[1]` | `0xB0` | 4 | 0x005551E4 | [L481](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L481) |
| `doClashExtraHitstop` | `DWORD[1]` | `0xB4` | 4 | 0x005551E8 | [L482](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L482) |
| `health` | `DWORD[1]` | `0xB8` | 4 | 0x005551EC | [L483](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L483) |
| `redHealth` | `DWORD[1]` | `0xBC` | 4 | 0x005551F0 | [L484](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L484) |
| `guardGauge` | `float[1]` | `0xC0` | 4 | 0x005551F4 | [L485](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L485) |
| `guardGaugeHeal` | `float[1]` | `0xC4` | 4 | 0x005551F8 | [L486](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L486) |
| `guardGaugeState` | `DWORD[1]` | `0xC8` | 4 | 0x005551FC | [L487](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L487) |
| `guardGaugeStop` | `DWORD[1]` | `0xCC` | 4 | 0x00555200 | [L488](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L488) |
| `onBlockComboCount` | `WORD[1]` | `0xD0` | 2 | 0x00555204 | [L489](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L489) |
| `guardQualityStop` | `WORD[1]` | `0xD2` | 2 | 0x00555206 | [L490](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L490) |
| `quardQuality` | `float[1]` | `0xD4` | 4 | 0x00555208 | [L491](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L491) |
| `exGuardMeterPenaltyTimer` | `WORD[1]` | `0xD8` | 2 | 0x0055520C | [L492](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L492) |
| `magicCircuit` | `DWORD[1]` | `0xDC` | 4 | 0x00555210 | [L494](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L494) |
| `heatTimeLeft` | `DWORD[1]` | `0xE0` | 4 | 0x00555214 | [L495](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L495) |
| `magicCircuitState` | `BYTE[1]` | `0xE4` | 1 | 0x00555218 | [L496](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L496) |
| `maxHeatTime` | `WORD[1]` | `0xE8` | 2 | 0x0055521C | [L498](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L498) |
| `magicCircuitPause` | `WORD[1]` | `0xEA` | 2 | 0x0055521E | [L499](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L499) |
| `heatTimeCounter` | `DWORD[1]` | `0xEC` | 4 | 0x00555220 | [L500](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L500) |
| `storedMeterUsage` | `DWORD[1]` | `0xF4` | 4 | 0x00555228 | [L502](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L502) |
| `storedMeterCircuitState` | `DWORD[1]` | `0xF8` | 4 | 0x0055522C | [L503](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L503) |
| `meterMultTimer` | `WORD[1]` | `0xFC` | 2 | 0x00555230 | [L504](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L504) |
| `meterMultTimerTotal` | `WORD[1]` | `0xFE` | 2 | 0x00555232 | [L505](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L505) |
| `meterGainMultiplier` | `WORD[1]` | `0x100` | 2 | 0x00555234 | [L506](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L506) |
| `xPos` | `int[1]` | `0x104` | 4 | 0x00555238 | [L508](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L508) |
| `yPos` | `int[1]` | `0x108` | 4 | 0x0055523C | [L509](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L509) |
| `prevXPos` | `int[1]` | `0x110` | 4 | 0x00555244 | [L511](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L511) |
| `prevYPos` | `int[1]` | `0x114` | 4 | 0x00555248 | [L512](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L512) |
| `xVel` | `int[1]` | `0x118` | 4 | 0x0055524C | [L513](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L513) |
| `yVel` | `int[1]` | `0x11C` | 4 | 0x00555250 | [L514](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L514) |
| `xAccel` | `short[1]` | `0x120` | 2 | 0x00555254 | [L515](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L515) |
| `yAccel` | `short[1]` | `0x122` | 2 | 0x00555256 | [L516](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L516) |
| `maxXSpeed` | `short[1]` | `0x124` | 2 | 0x00555258 | [L517](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L517) |
| `xVelChange` | `int[1]` | `0x128` | 4 | 0x0055525C | [L519](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L519) |
| `yVelChange` | `int[1]` | `0x12C` | 4 | 0x00555260 | [L520](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L520) |
| `xAccelChange` | `short[1]` | `0x130` | 2 | 0x00555264 | [L521](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L521) |
| `yAccelChange` | `short[1]` | `0x132` | 2 | 0x00555266 | [L522](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L522) |
| `momentum` | `int[1]` | `0x134` | 4 | 0x00555268 | [L523](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L523) |
| `momentumQueue` | `int[1]` | `0x138` | 4 | 0x0055526C | [L524](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L524) |
| `momentumDecay` | `short[1]` | `0x13C` | 2 | 0x00555270 | [L525](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L525) |
| `momentumGrantingPattern` | `short[1]` | `0x13E` | 2 | 0x00555272 | [L526](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L526) |
| `useAddYMaxXParams` | `short[1]` | `0x140` | 2 | 0x00555274 | [L527](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L527) |
| `thrownXOffset` | `int[1]` | `0x154` | 4 | 0x00555288 | [L529](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L529) |
| `thrownYOffset` | `int[1]` | `0x158` | 4 | 0x0055528C | [L530](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L530) |
| `hitType` | `short[1]` | `0x15C` | 2 | 0x00555290 | [L531](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L531) |
| `projectileHitType` | `short[1]` | `0x15E` | 2 | 0x00555292 | [L532](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L532) |
| `zPriority` | `short[1]` | `0x160` | 2 | 0x00555294 | [L533](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L533) |
| `zPrioritySetter` | `short[1]` | `0x162` | 2 | 0x00555296 | [L534](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L534) |
| `shieldHeldTime` | `int[1]` | `0x168` | 4 | 0x0055529C | [L536](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L536) |
| `shieldSuccessType` | `BYTE[1]` | `0x16C` | 1 | 0x005552A0 | [L537](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L537) |
| `autoSuperJump` | `BYTE[1]` | `0x16D` | 1 | 0x005552A1 | [L538](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L538) |
| `hitstop` | `BYTE[1]` | `0x16E` | 1 | 0x005552A2 | [L539](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L539) |
| `hitstopAdvanceFrames` | `BYTE[1]` | `0x16F` | 1 | 0x005552A3 | [L540](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L540) |
| `ungrantedHitstop` | `BYTE[1]` | `0x171` | 1 | 0x005552A5 | [L542](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L542) |
| `throwFlag` | `BYTE[1]` | `0x172` | 1 | 0x005552A6 | [L543](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L543) |
| `noInputFlag` | `BYTE[1]` | `0x173` | 1 | 0x005552A7 | [L544](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L544) |
| `tagFlag` | `BYTE[1]` | `0x174` | 1 | 0x005552A8 | [L545](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L545) |
| `remainingHits` | `BYTE[1]` | `0x176` | 1 | 0x005552AA | [L547](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L547) |
| `inBlockstun` | `BYTE[1]` | `0x177` | 1 | 0x005552AB | [L548](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L548) |
| `willBlock` | `BYTE[1]` | `0x178` | 1 | 0x005552AC | [L549](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L549) |
| `proxyGuardTime` | `BYTE[1]` | `0x179` | 1 | 0x005552AD | [L550](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L550) |
| `bounceCount` | `BYTE[1]` | `0x17A` | 1 | 0x005552AE | [L551](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L551) |
| `comboJumpCancel` | `BYTE[1]` | `0x17D` | 1 | 0x005552B1 | [L553](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L553) |
| `burstLock` | `BYTE[1]` | `0x17E` | 1 | 0x005552B2 | [L554](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L554) |
| `strikeInvuln` | `BYTE[1]` | `0x181` | 1 | 0x005552B5 | [L556](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L556) |
| `throwInvuln` | `BYTE[1]` | `0x182` | 1 | 0x005552B6 | [L557](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L557) |
| `preJumpThrowInvuln` | `BYTE[1]` | `0x184` | 1 | 0x005552B8 | [L559](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L559) |
| `airTime` | `WORD[1]` | `0x186` | 2 | 0x005552BA | [L561](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L561) |
| `timeThrown` | `WORD[1]` | `0x188` | 2 | 0x005552BC | [L562](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L562) |
| `totalUntechTime` | `WORD[1]` | `0x18A` | 2 | 0x005552BE | [L563](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L563) |
| `untechTimeElapsed` | `WORD[1]` | `0x18C` | 2 | 0x005552C0 | [L564](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L564) |
| `canAirTech` | `BYTE[1]` | `0x190` | 1 | 0x005552C4 | [L566](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L566) |
| `canGroundTech` | `BYTE[1]` | `0x191` | 1 | 0x005552C5 | [L567](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L567) |
| `isGroundTech` | `BYTE[1]` | `0x192` | 1 | 0x005552C6 | [L568](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L568) |
| `armorTimer` | `WORD[1]` | `0x194` | 2 | 0x005552C8 | [L570](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L570) |
| `reversedControlsTimer` | `WORD[1]` | `0x196` | 2 | 0x005552CA | [L571](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L571) |
| `receivedHitstop` | `BYTE[1]` | `0x1A0` | 1 | 0x005552D4 | [L573](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L573) |
| `numOverlapHitboxes` | `BYTE[1]` | `0x1A1` | 1 | 0x005552D5 | [L574](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L574) |
| `hitstunBlockstunTimeElapsed` | `DWORD[1]` | `0x1A4` | 4 | 0x005552D8 | [L576](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L576) |
| `hitstunTimeRemaining` | `int[1]` | `0x1A8` | 4 | 0x005552DC | [L577](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L577) |
| `completedHitVectors` | `BYTE[1]` | `0x1AC` | 1 | 0x005552E0 | [L578](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L578) |
| `didHitVectorFaceLeft` | `BYTE[1]` | `0x1AD` | 1 | 0x005552E1 | [L579](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L579) |
| `receivedHitVector` | `BYTE[1]` | `0x1AE` | 1 | 0x005552E2 | [L580](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L580) |
| `someDeathFlagMaybe` | `BYTE[1]` | `0x1B1` | 1 | 0x005552E5 | [L582](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L582) |
| `jumpVariable` | `BYTE[1]` | `0x1B3` | 1 | 0x005552E7 | [L584](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L584) |
| `specialVariables` | `BYTE[10]` | `0x1B4` | 10 | 0x005552E8 | [L585](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L585) |
| `dashVariable` | `BYTE[1]` | `0x1BE` | 1 | 0x005552F2 | [L586](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L586) |
| `extraVariables` | `WORD[10]` | `0x1C0` | 20 | 0x005552F4 | [L588](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L588) |
| `xVelStorage` | `WORD[1]` | `0x1D4` | 2 | 0x00555308 | [L589](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L589) |
| `yVelStorage` | `WORD[1]` | `0x1D6` | 2 | 0x0055530A | [L590](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L590) |
| `xAccStorage` | `WORD[1]` | `0x1D8` | 2 | 0x0055530C | [L591](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L591) |
| `yAccStorage` | `WORD[1]` | `0x1DA` | 2 | 0x0055530E | [L592](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L592) |
| `afterImageBlendMode` | `BYTE[1]` | `0x1E2` | 1 | 0x00555316 | [L594](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L594) |
| `numAfterImages` | `BYTE[1]` | `0x1E3` | 1 | 0x00555317 | [L595](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L595) |
| `afterImageFramePosOffset` | `BYTE[1]` | `0x1E4` | 1 | 0x00555318 | [L596](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L596) |
| `reversePenalty` | `WORD[1]` | `0x1E6` | 2 | 0x0055531A | [L598](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L598) |
| `reversePenaltyDecayTimer` | `WORD[1]` | `0x1E8` | 2 | 0x0055531C | [L599](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L599) |
| `onHitComboCount` | `WORD[1]` | `0x1EA` | 2 | 0x0055531E | [L600](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L600) |
| `unknownHitCounter` | `WORD[1]` | `0x1EC` | 2 | 0x00555320 | [L601](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L601) |
| `counterhitState` | `BYTE[1]` | `0x1F6` | 1 | 0x0055532A | [L603](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L603) |
| `receivingAttackDataPtrArr` | `AttackData*[8]` | `0x1F8` | 32 | 0x0055532C | [L605](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L605) |
| `hitboxOverlapArr` | `CameraBoxData[8]` | `0x218` | 128 | 0x0055534C | [L606](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L606) |
| `attackingSubObjPtrArr` | `ActorData*[8]` | `0x298` | 32 | 0x005553CC | [L607](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L607) |
| `lastHitBySubObjPtr` | `ActorData*[1]` | `0x2B8` | 4 | 0x005553EC | [L608](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L608) |
| `lastHitSubObjPtr` | `ActorData*[1]` | `0x2BC` | 4 | 0x005553F0 | [L609](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L609) |
| `isControllingSubObjPtr` | `ActorData*[1]` | `0x2C0` | 4 | 0x005553F4 | [L610](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L610) |
| `isControllingActor` | `BYTE[1]` | `0x2C4` | 1 | 0x005553F8 | [L611](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L611) |
| `OwnerSubObjPtr` | `ActorData*[1]` | `0x2C8` | 4 | 0x005553FC | [L613](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L613) |
| `isControlledBySubObjPtr` | `ActorData*[1]` | `0x2CC` | 4 | 0x00555400 | [L614](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L614) |
| `type2FlashSpawnedDuring` | `int[1]` | `0x2D8` | 4 | 0x0055540C | [L616](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L616) |
| `gravity` | `float[1]` | `0x2E0` | 4 | 0x00555414 | [L618](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L618) |
| `untechPenalty` | `WORD[1]` | `0x2E4` | 2 | 0x00555418 | [L619](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L619) |
| `correctedDirInput` | `BYTE[1]` | `0x2E6` | 1 | 0x0055541A | [L620](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L620) |
| `rawDirInput` | `BYTE[1]` | `0x2E7` | 1 | 0x0055541B | [L621](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L621) |
| `buttonInputs` | `DWORD[1]` | `0x2E8` | 4 | 0x0055541C | [L622](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L622) |
| `buttonReleased` | `DWORD[1]` | `0x2EC` | 4 | 0x00555420 | [L623](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L623) |
| `ownerIndex` | `BYTE[1]` | `0x2F0` | 1 | 0x00555424 | [L624](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L624) |
| `queueDespawn` | `BYTE[1]` | `0x2F1` | 1 | 0x00555425 | [L625](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L625) |
| `numSpawnedEffects` | `WORD[1]` | `0x2F2` | 2 | 0x00555426 | [L626](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L626) |
| `grabLocX` | `int[1]` | `0x2F4` | 4 | 0x00555428 | [L627](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L627) |
| `grabLocY` | `int[1]` | `0x2F8` | 4 | 0x0055542C | [L628](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L628) |
| `spriteRotation` | `int[1]` | `0x2FC` | 4 | 0x00555430 | [L629](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L629) |
| `xScale` | `float[1]` | `0x300` | 4 | 0x00555434 | [L630](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L630) |
| `yScale` | `float[1]` | `0x304` | 4 | 0x00555438 | [L631](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L631) |
| `targetPattern` | `WORD[1]` | `0x308` | 2 | 0x0055543C | [L632](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L632) |
| `targetPatternPriority` | `WORD[1]` | `0x30A` | 2 | 0x0055543E | [L633](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L633) |
| `targetState` | `WORD[1]` | `0x30C` | 2 | 0x00555440 | [L634](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L634) |
| `targetStatePriority` | `WORD[1]` | `0x30E` | 2 | 0x00555442 | [L635](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L635) |
| `facingLeft` | `BYTE[1]` | `0x310` | 1 | 0x00555444 | [L636](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L636) |
| `isOpponentToLeft` | `BYTE[1]` | `0x311` | 1 | 0x00555445 | [L637](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L637) |
| `needToCrossupInputs` | `BYTE[1]` | `0x312` | 1 | 0x00555446 | [L638](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L638) |
| `justShielded` | `BYTE[1]` | `0x313` | 1 | 0x00555447 | [L639](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L639) |
| `justEnteredNewPattern` | `BYTE[1]` | `0x314` | 1 | 0x00555448 | [L640](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L640) |
| `doLanding` | `BYTE[1]` | `0x315` | 1 | 0x00555449 | [L641](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L641) |
| `justLanded` | `BYTE[1]` | `0x316` | 1 | 0x0055544A | [L642](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L642) |
| `delayedStance` | `BYTE[1]` | `0x317` | 1 | 0x0055544B | [L643](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L643) |
| `patternDataPtr` | `PatternData*[1]` | `0x318` | 4 | 0x0055544C | [L644](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L644) |
| `animationDataPtr` | `AnimationData*[1]` | `0x31C` | 4 | 0x00555450 | [L645](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L645) |
| `attackDataPtr` | `AttackData*[1]` | `0x320` | 4 | 0x00555454 | [L646](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L646) |
| `selfPtr` | `EffectData*[1]` | `0x324` | 4 | 0x00555458 | [L647](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L647) |
| `partnerPtr` | `ActorData*[1]` | `0x328` | 4 | 0x0055545C | [L648](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L648) |
| `charFileDataPtr` | `CharFileData*[1]` | `0x32C` | 4 | 0x00555460 | [L649](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L649) |
| `framesIntoCurrentPattern` | `DWORD[1]` | `0x330` | 4 | 0x00555464 | [L650](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L650) |
| `didAdvanceFramesIntoCurrentPattern` | `BYTE[1]` | `0x334` | 1 | 0x00555468 | [L651](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L651) |

## PlayerData（0xAFC bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `cmdFileDataPtr` | `CommandFileData*[1]` | `0x33C` | 4 | 0x0055546C | [L692](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L692) |
| `reduceFlag` | `int[1]` | `0x348` | 4 | 0x00555478 | [L694](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L694) |
| `reduceCounter` | `int[1]` | `0x34C` | 4 | 0x0055547C | [L695](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L695) |
| `usedNormalsInString` | `int[1]` | `0x354` | 4 | 0x00555484 | [L697](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L697) |
| `inputCmdID` | `int[1]` | `0x3D8` | 4 | 0x00555508 | [L699](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L699) |
| `chainedCmdsCounter` | `int[1]` | `0x3E0` | 4 | 0x00555510 | [L701](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L701) |
| `dirInputs` | `WORD[129]` | `0x3E8` | 258 | 0x00555518 | [L703](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L703) |
| `aInputs` | `WORD[129]` | `0x4EA` | 258 | 0x0055561A | [L704](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L704) |
| `bInputs` | `WORD[129]` | `0x5EC` | 258 | 0x0055571C | [L705](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L705) |
| `cInputs` | `WORD[129]` | `0x6EE` | 258 | 0x0055581E | [L706](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L706) |
| `dInputs` | `WORD[129]` | `0x7F0` | 258 | 0x00555920 | [L707](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L707) |
| `eInputs` | `WORD[129]` | `0x8F2` | 258 | 0x00555A22 | [L708](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L708) |
| `fInputs` | `WORD[129]` | `0x9F4` | 258 | 0x00555B24 | [L709](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L709) |

## AttackDisplayData（0x18 bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `comboInvalid` | `int[1]` | `0x0` | 4 | 動的／親構造体基点 | [L725](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L725) |
| `comboTrue` | `int[1]` | `0x4` | 4 | 動的／親構造体基点 | [L726](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L726) |
| `damageScaled` | `int[1]` | `0x8` | 4 | 動的／親構造体基点 | [L727](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L727) |
| `damageUnscaled` | `int[1]` | `0xC` | 4 | 動的／親構造体基点 | [L728](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L728) |
| `vsDamage` | `int[1]` | `0x10` | 4 | 動的／親構造体基点 | [L729](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L729) |
| `meterGain` | `int[1]` | `0x14` | 4 | 動的／親構造体基点 | [L730](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L730) |

## ComboCalcData（0x2C bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `index` | `byte[1]` | `0x0` | 1 | 動的／親構造体基点 | [L736](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L736) |
| `field_0x2` | `short[1]` | `0x1` | 2 | 動的／親構造体基点 | [L737](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L737) |
| `numHits` | `int[1]` | `0x3` | 4 | 動的／親構造体基点 | [L738](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L738) |
| `damage` | `int[1]` | `0x7` | 4 | 動的／親構造体基点 | [L739](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L739) |
| `isInvalid` | `short[1]` | `0xB` | 2 | 動的／親構造体基点 | [L740](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L740) |
| `field_0xe` | `short[1]` | `0xD` | 2 | 動的／親構造体基点 | [L741](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L741) |
| `proration` | `short[1]` | `0xF` | 2 | 動的／親構造体基点 | [L742](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L742) |
| `otgMeterMult` | `short[1]` | `0x11` | 2 | 動的／親構造体基点 | [L743](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L743) |
| `drawComboData` | `byte[1]` | `0x13` | 1 | 動的／親構造体基点 | [L744](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L744) |
| `field_0x15` | `byte[1]` | `0x14` | 1 | 動的／親構造体基点 | [L745](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L745) |
| `timer1` | `short[1]` | `0x15` | 2 | 動的／親構造体基点 | [L746](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L746) |
| `timer2` | `short[1]` | `0x17` | 2 | 動的／親構造体基点 | [L747](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L747) |
| `field_0x1a` | `short[1]` | `0x19` | 2 | 動的／親構造体基点 | [L748](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L748) |
| `someFlag` | `int[1]` | `0x1B` | 4 | 動的／親構造体基点 | [L749](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L749) |
| `timer3` | `int[1]` | `0x1F` | 4 | 動的／親構造体基点 | [L750](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L750) |
| `xPos` | `int[1]` | `0x23` | 4 | 動的／親構造体基点 | [L751](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L751) |
| `yPos` | `int[1]` | `0x27` | 4 | 動的／親構造体基点 | [L752](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L752) |
| `alpha` | `byte[1]` | `0x2B` | 1 | 動的／親構造体基点 | [L753](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L753) |

## PlayerAuxData（0x20C bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `activeCharacter` | `int[1]` | `0x0` | 4 | 0x00557DB8 | [L759](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L759) |
| `assistChangeState` | `int[1]` | `0x4` | 4 | 0x00557DBC | [L760](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L760) |
| `hasPartner` | `int[1]` | `0xC` | 4 | 0x00557DC4 | [L762](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L762) |
| `dispCorrectionValue` | `int[1]` | `0x20` | 4 | 0x00557DD8 | [L764](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L764) |
| `someDamageMult` | `float[1]` | `0x3C` | 4 | 0x00557DF4 | [L766](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L766) |
| `comboCount` | `int[1]` | `0x44` | 4 | 0x00557DFC | [L768](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L768) |
| `comboCountCopy` | `int[1]` | `0x48` | 4 | 0x00557E00 | [L769](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L769) |
| `dispMaxCombo` | `int[1]` | `0x50` | 4 | 0x00557E08 | [L771](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L771) |
| `maxDamage` | `int[1]` | `0x54` | 4 | 0x00557E0C | [L772](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L772) |
| `maxCombo` | `int[1]` | `0x58` | 4 | 0x00557E10 | [L773](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L773) |
| `nextAttackDisplayIndex` | `int[1]` | `0x64` | 4 | 0x00557E1C | [L775](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L775) |
| `currentAttackDisplayIndex` | `int[1]` | `0x68` | 4 | 0x00557E20 | [L776](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L776) |
| `attackDisplayData` | `AttackDisplayData[2]` | `0x70` | 48 | 0x00557E28 | [L778](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L778) |
| `comboCalcIndex` | `byte[1]` | `0xA0` | 1 | 0x00557E58 | [L779](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L779) |
| `comboCalcData` | `ComboCalcData[8]` | `0xA1` | 352 | 0x00557E59 | [L780](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L780) |
| `dispCHTimer` | `int[1]` | `0x204` | 4 | 0x00557FBC | [L782](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L782) |
| `inactionableFrames` | `int[1]` | `0x208` | 4 | 0x00557FC0 | [L783](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L783) |

## CommandData（0x2C bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `ID` | `int[1]` | `0x0` | 4 | 動的／親構造体基点 | [L364](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L364) |
| `input` | `char[20]` | `0x4` | 20 | 動的／親構造体基点 | [L365](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L365) |
| `pattern` | `int[1]` | `0x18` | 4 | 動的／親構造体基点 | [L366](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L366) |
| `specialFlag` | `int[1]` | `0x1C` | 4 | 動的／親構造体基点 | [L367](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L367) |
| `meterSpend` | `int[1]` | `0x20` | 4 | 動的／親構造体基点 | [L368](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L368) |
| `assistVar` | `byte[1]` | `0x24` | 1 | 動的／親構造体基点 | [L369](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L369) |
| `specialVar` | `byte[1]` | `0x25` | 1 | 動的／親構造体基点 | [L370](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L370) |
| `dashVar` | `byte[1]` | `0x26` | 1 | 動的／親構造体基点 | [L371](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L371) |
| `flagsets` | `uint8_t[4]` | `0x28` | 4 | 動的／親構造体基点 | [L373](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L373) |

## CommandFileData（0x68 bytes）

| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |
|---|---|---|---|---|---|
| `cmdDataPtr` | `ArrayContainer<CommandData*>*[1]` | `0x0` | 4 | 動的／親構造体基点 | [L386](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L386) |
| `airJumpNum` | `byte[1]` | `0x4` | 1 | 動的／親構造体基点 | [L387](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L387) |
| `Flags` | `byte[1]` | `0x9` | 1 | 動的／親構造体基点 | [L389](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L389) |
| `KoRareVoice` | `byte[1]` | `0xB` | 1 | 動的／親構造体基点 | [L391](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L391) |
| `CharaGravity_AddY` | `int[1]` | `0x10` | 4 | 動的／親構造体基点 | [L393](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L393) |
| `CharaGravity_MaxX` | `int[1]` | `0x14` | 4 | 動的／親構造体基点 | [L394](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L394) |
| `guts` | `float[4]` | `0x18` | 16 | 動的／親構造体基点 | [L395](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L395) |
| `ShieldCounter_Ground` | `int[1]` | `0x30` | 4 | 動的／親構造体基点 | [L397](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L397) |
| `ShieldCounter_Air` | `int[1]` | `0x34` | 4 | 動的／親構造体基点 | [L398](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L398) |
| `ShieldCounter_Crouch` | `int[1]` | `0x38` | 4 | 動的／親構造体基点 | [L399](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L399) |
| `CancelSparkPat_Ground` | `int[1]` | `0x44` | 4 | 動的／親構造体基点 | [L401](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L401) |
| `CancelSparkPat_Air` | `int[1]` | `0x48` | 4 | 動的／親構造体基点 | [L402](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L402) |
| `CancelHighJumpPat` | `int[1]` | `0x4C` | 4 | 動的／親構造体基点 | [L403](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L403) |
| `isGroundThrowDefined` | `short[1]` | `0x50` | 2 | 動的／親構造体基点 | [L404](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L404) |
| `groundThrowPat` | `short[1]` | `0x52` | 2 | 動的／親構造体基点 | [L405](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L405) |
| `groundThrowRange` | `short[1]` | `0x54` | 2 | 動的／親構造体基点 | [L406](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L406) |
| `isAirThrowDefined` | `short[1]` | `0x56` | 2 | 動的／親構造体基点 | [L407](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L407) |
| `airThrowPat` | `short[1]` | `0x58` | 2 | 動的／親構造体基点 | [L408](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L408) |
| `airThrowRange` | `short[1]` | `0x5A` | 2 | 動的／親構造体基点 | [L409](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L409) |
| `KoAniEff` | `int[1]` | `0x5C` | 4 | 動的／親構造体基点 | [L410](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L410) |
| `ExComCheck_Num` | `int[1]` | `0x60` | 4 | 動的／親構造体基点 | [L411](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L411) |
| `ExComCheckPtr` | `void*[1]` | `0x64` | 4 | 動的／親構造体基点 | [L412](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L412) |
