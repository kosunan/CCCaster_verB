#!/usr/bin/env bash
# ============================================================================
# run_pair.sh — run_pair.ps1 の Linux 版
#
#   ./src/harness/run_pair.sh
#   ./src/harness/run_pair.sh --host-loading 60 --client-loading 180
#
# ロード時間を左右で変えると、証言②（ロード時間のばらつきでずれる）を再現できる。
# 記録は build_logs/harness/ に出力される。
#
# 判定・出力形式は run_pair.ps1 と同じに保つこと。**両OSで同じ物差しで測れないと、
# 「Linux では OK だが実機では NG」の切り分けができなくなる**ため。
# ============================================================================
set -uo pipefail

HOST_LOADING=60
CLIENT_LOADING=60
ROUNDS=2
HOST_PORT=7600
CLIENT_PORT=7601
TIMEOUT_SECONDS=90
TIME_SCALE=1     # 時間圧縮。4 なら4倍速（区切りの確認は必ず 1 で）

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-loading)   HOST_LOADING="$2";   shift 2 ;;
        --client-loading) CLIENT_LOADING="$2"; shift 2 ;;
        --rounds)         ROUNDS="$2";         shift 2 ;;
        --host-port)      HOST_PORT="$2";      shift 2 ;;
        --client-port)    CLIENT_PORT="$2";    shift 2 ;;
        --timeout)        TIMEOUT_SECONDS="$2";shift 2 ;;
        --time-scale)     TIME_SCALE="$2";     shift 2 ;;
        --build-dir)      BUILD_DIR="$2";      shift 2 ;;
        *) echo "不明な引数: $1" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
EXE="$BUILD_DIR/bin/harness"
OUT_DIR="$ROOT/build_logs/harness"

if [[ ! -x "$EXE" ]]; then
    echo "harness がありません: $EXE"
    echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi
mkdir -p "$OUT_DIR"

export CCCASTER_TIME_SCALE="$TIME_SCALE"
if [[ "$TIME_SCALE" -gt 1 ]]; then
    echo "[pair] 時間圧縮 x$TIME_SCALE（タイミング余裕の検証にはならない）"
fi

HOST_REC="$OUT_DIR/host_record.txt"
CLIENT_REC="$OUT_DIR/client_record.txt"
HOST_LOG="$OUT_DIR/host.log"
CLIENT_LOG="$OUT_DIR/client.log"
rm -f "$HOST_REC"* "$CLIENT_REC"* "$HOST_LOG" "$CLIENT_LOG"

echo "[pair] HOST   loading=${HOST_LOADING}F  local=$HOST_PORT   peer=$CLIENT_PORT"
echo "[pair] CLIENT loading=${CLIENT_LOADING}F  local=$CLIENT_PORT  peer=$HOST_PORT"

"$EXE" --host --ip 127.0.0.1 \
       --port "$CLIENT_PORT" --local-port "$HOST_PORT" \
       --loading-frames "$HOST_LOADING" --rounds "$ROUNDS" \
       --out "$HOST_REC" > "$HOST_LOG" 2>&1 &
HOST_PID=$!

sleep 0.5

"$EXE" --ip 127.0.0.1 \
       --port "$HOST_PORT" --local-port "$CLIENT_PORT" \
       --loading-frames "$CLIENT_LOADING" --rounds "$ROUNDS" \
       --out "$CLIENT_REC" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

echo "[pair] 起動しました。最大 ${TIMEOUT_SECONDS} 秒待機します..."
for _ in $(seq 1 "$TIMEOUT_SECONDS"); do
    sleep 1
    kill -0 "$HOST_PID" 2>/dev/null || kill -0 "$CLIENT_PID" 2>/dev/null || break
done
# 片方だけ生きている場合も含めて確実に落とす
kill -TERM "$HOST_PID" "$CLIENT_PID" 2>/dev/null
sleep 2
kill -KILL "$HOST_PID" "$CLIENT_PID" 2>/dev/null
wait "$HOST_PID" "$CLIENT_PID" 2>/dev/null

echo
echo '===== HOST (末尾8行) ====='
[[ -f "$HOST_LOG" ]]   && tail -n 8 "$HOST_LOG"
echo
echo '===== CLIENT (末尾8行) ====='
[[ -f "$CLIENT_LOG" ]] && tail -n 8 "$CLIENT_LOG"

python3 - "$HOST_REC" "$CLIENT_REC" <<'PY'
import sys, os

host_rec, client_rec = sys.argv[1], sys.argv[2]

def read_record(path):
    """netFrame -> "p1dir p1btn p2dir p2btn" """
    m = {}
    if not os.path.exists(path):
        return m
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.split()
            if len(p) < 5:
                continue
            m[int(p[0])] = ' '.join(p[1:5])
    return m

def read_state(path):
    """netFrame -> 列配列。同一 netFrame は最初を採用（実機の [MEM] 判定と同じ）"""
    m = {}
    if not os.path.exists(path):
        return m
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            a = line.split()
            if len(a) < 5:
                continue
            m.setdefault(int(a[0]), a)
    return m

print()
print('===== 決定性チェック =====')
h, c = read_record(host_rec), read_record(client_rec)
if not h or not c:
    print('  記録が揃っていません')
    print('DONE')
    sys.exit(1)

print(f'  host  : {len(h)} フレーム記録')
print(f'  client: {len(c)} フレーム記録')

common = sorted(set(h) & set(c))
mismatch = [f for f in common if h[f] != c[f]]
print(f'  共通フレーム: {len(common)}')
if not common:
    print('  [NG] 突き合わせ可能なフレームがありません')
elif not mismatch:
    print('  [OK] 共通フレームの入力列は完全に一致')
else:
    print(f'  [NG] {len(mismatch)} フレームで不一致。先頭5件:')
    for f in mismatch[:5]:
        print(f'    netFrame={f}  host=[{h[f]}]  client=[{c[f]}]')

print()
print('===== ゲーム状態の突き合わせ =====')
# 入力列が一致していても、ここがずれていれば実際の対戦はデシンクする。
sh, sc = read_state(host_rec + '.state'), read_state(client_rec + '.state')
if not sh or not sc:
    print('  状態記録がありません')
else:
    cols = ['mode', 'intro', 'WT', 'RT']
    skeys = sorted(set(sh) & set(sc))
    print(f'  共通 netFrame: {len(skeys)}')
    for i, col in enumerate(cols):
        idx = i + 1
        first = next((k for k in skeys
                      if idx < len(sh[k]) and idx < len(sc[k]) and sh[k][idx] != sc[k][idx]), None)
        if first is None:
            print(f'    {col:<6}: 一致')
        else:
            print(f'    {col:<6}: netFrame={first} で分岐 '
                  f'(host={sh[first][idx]} client={sc[first][idx]})')
PY

echo
echo '===== stall / conflict ====='
for pair in "host:$HOST_LOG" "client:$CLIENT_LOG"; do
    name="${pair%%:*}"; log="${pair#*:}"
    echo "  $name: $(grep 'stall=' "$log" 2>/dev/null | tail -n 1)"
done
echo 'DONE'
