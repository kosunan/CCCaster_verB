"""番号付き採取値の公開境界から、同じフレームのゲーム入力注入までを実QPCで照合する。"""
import argparse
import json
from pathlib import Path
import re
from analyze_deadline_diagnostics import distribution


def analyze(text, confirmed, minimum=600):
    tables = {k: {} for k in ('InputCaptureSend','InputHandoff','FramePipeline','FrameStart','UpdateCadence')}
    presents = []
    errors = []
    for line in text.splitlines():
        tag = re.match(r'\[(\w+)\]',line)
        if not tag: continue
        name = tag[1]
        if name not in tables and name != 'MonitorPresent': continue
        row = {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)}
        if name == 'MonitorPresent': presents.append(row); continue
        if row['f'] in tables[name]: errors.append(f'{name}: duplicate {row["f"]}')
        tables[name][row['f']] = row
    rows = []
    for frame,h in tables['InputHandoff'].items():
        u = tables['UpdateCadence'].get(frame,{})
        if frame > int(confirmed.get(str(frame//65536),0)) or not u.get('play') or not u.get('consecutive'):
            continue
        try:
            p = tables['FramePipeline'][frame]
            c = tables['InputCaptureSend'][h['source']]
            s = tables['FrameStart'][frame]
            assert h['simulationDue']-h['captureDue'] == 60*h['phaseUs'], 'phase no longer anchored to capture clock'
            assert c['begin'] <= c['end'] <= c['published'] <= p['input'] <= p['saved'] <= p['prepared'] <= p['native'] <= p['present'], 'input, save, injection, native ordering'
            assert p['start'] == s['actual'] == u['ticks'], 'frame entry mismatch'
            assert u['dropped'] == 0, 'dropped samples'
            rows.append(dict(frame=frame,source=h['source'],capture=h['capture'],phase_us=h['phaseUs'],
                notified=bool(h.get('notified',0)),
                capture_to_inject_us=(p['prepared']-c['begin'])/60,
                ready_to_inject_us=(p['prepared']-c['end'])/60,
                publish_marker_to_inject_us=(p['prepared']-c['published'])/60,
                publish_marker_to_native_us=(p['native']-c['published'])/60,
                publish_marker_to_render_us=(p['present']-c['published'])/60,
                start_interval_us=u['interval']/60,
                arrival_late_us=s['readyLate']/60,
                final_wait_overrun_us=(s['actual']-s['due'])/60))
        except (KeyError,AssertionError) as exc:
            errors.append(f'{frame}: {exc}')
    if len(rows)<minimum: errors.append(f'insufficient samples: {len(rows)} < {minimum}')
    fields=('capture_to_inject_us','ready_to_inject_us','publish_marker_to_inject_us',
            'publish_marker_to_native_us','publish_marker_to_render_us','start_interval_us','arrival_late_us','final_wait_overrun_us')
    active_frames={r['frame'] for r in rows}
    active_presents=[p for p in presents if p.get('frame') in active_frames and p.get('mode')==1 and p.get('intro')==0]
    return dict(passed=not errors,errors=errors,samples=len(rows),
                phase_us=sorted({r['phase_us'] for r in rows}),
                notification_modes=sorted({r['notified'] for r in rows}),
                stats={key:distribution([r[key] for r in rows]) for key in fields},
                arrival_after_deadline=sum(r['arrival_late_us']>0 for r in rows),
                final_wait_overrun_3us=sum(r['final_wait_overrun_us']>3 for r in rows),
                interval_outside_3us=sum(abs(r['start_interval_us']-1e6/60)>3 for r in rows),
                presents=len(active_presents),repeat_presents=sum(p['repeat'] for p in active_presents),
                worst=sorted(rows,key=lambda r:r['publish_marker_to_inject_us'],reverse=True)[:8],
                scope='Actual QPC, confirmed combat only. Publication marker precedes WriteLocal; includes publication cost. Render means final render before Present, not input response or monitor scanout. Final-wait overrun excludes late arrival: its projected QPC deadline is clamped to arrival when already late; see arrival_late_us separately.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    result=json.loads((args.directory/'result.json').read_text(encoding='utf-8'))
    confirmed=result['sync']['confirmed_by_epoch']
    reports={str(side):analyze((args.directory/f'game_{side}.log').read_text(encoding='utf-8'),confirmed) for side in (1,2)}
    report=dict(source_passed=result['passed'],sides=reports,
                passed=result['passed'] and all(r['passed'] for r in reports.values()))
    (args.directory/'input_handoff.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))
    return int(not report['passed'])


if __name__=='__main__':raise SystemExit(main())
