"""デバッガ採取をSpinOS/ReplayWorkと照合し、実メモリの逆アセンブルを保存。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import statistics
from analyze_spin_etw import read_spin, IntervalIndex


def load_debug(path):
    modules, samples, events, errors = [], [], [], []
    meta, ended = None, False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        p = line.split("\t")
        try:
            if p[0] == "META":
                meta = dict(pid=int(p[2]), tid=int(p[3]), hz=int(p[4]), nominal_interval_us=int(p[5]))
            elif p[0] == "MODULE":
                modules.append(dict(begin=int(p[1]), end=None, base=int(p[2]), size=int(p[3]), stamp=int(p[4]), path=p[5]))
            elif p[0] == "UNLOAD":
                for m in reversed(modules):
                    if m['base'] == int(p[2]) and m['end'] is None:
                        m['end'] = int(p[1]); break
            elif p[0] == "SAMPLE":
                if len(p) != 17: raise ValueError("incomplete sample")
                row = dict(zip(('begin','captured','end','previous_suspend','eip','esp','ebp','eax','ebx','ecx','edx','esi','edi','eflags'), map(int,p[1:15])))
                row.update(code=p[15], stack=p[16]); bytes.fromhex(p[15]); bytes.fromhex(p[16]); samples.append(row)
            elif p[0] == "EVENT":
                events.append(dict(begin=int(p[1]), end=int(p[2]), event=int(p[3])))
            elif p[0] in ("ERROR", "LIMIT", "EXCEPTION"):
                errors.append(line)
            elif p[0] == "END": ended = True
        except (ValueError, IndexError): errors.append("malformed: " + line[:200])
    if not meta or meta['hz'] <= 0: raise ValueError(f"missing metadata: {path}")
    return meta, modules, samples, events, errors, ended


def resolve(modules, address, when):
    for m in reversed(modules):
        if m['begin'] <= when and (m['end'] is None or when < m['end']) and m['base'] <= address < m['base'] + m['size']:
            return dict(path=m['path'], base=m['base'], rva=address-m['base'], stamp=m['stamp'])
    return None


def frame_chain(row, modules):
    """採取したスタック範囲内の単調なEBP連鎖のみ。FPOを推測で補完しない。"""
    data=bytes.fromhex(row['stack'])
    pointer=row['ebp']; base=row['esp']; result=[]
    for _ in range(32):
        offset=pointer-base
        if offset<0 or offset+8>len(data) or offset%4: break
        previous,address=struct.unpack_from('<II',data,offset)
        result.append(dict(ebp=pointer,return_address=address,
                           module=resolve(modules,address,row['captured'])))
        if previous<=pointer: break
        pointer=previous
    return result


def load_owners(path, modules):
    owners=[]
    for line in path.read_text(encoding='utf-8',errors='replace').splitlines():
        parts=line.split('\t')
        if parts[0]!='OWNER': continue
        if len(parts)!=17: continue # 所有者再確認がない旧診断は確定所有者に含めない。
        keys=('main_captured','critical_section','tid','lock_count','recursion','begin','captured','end',
              'previous_suspend','error','eip','esp','ebp')
        try:
            row=dict(zip(keys,map(int,parts[1:14])))
            row.update(code=parts[14],stack=parts[15],owner_stable=parts[16]=='1')
            bytes.fromhex(row['code']);bytes.fromhex(row['stack'])
            row['module']=resolve(modules,row['eip'],row['captured'])
            row['ebp_chain']=frame_chain(row,modules)
            owners.append(row)
        except (ValueError,IndexError): continue
    return owners


def analyze(debug, game, threshold, frames=None, kinds=None):
    meta, modules, samples, events, errors, ended = load_debug(debug)
    windows = [w for w in read_spin(game, threshold) if w['pid']==meta['pid'] and w['tid']==meta['tid']]
    if frames: windows=[w for w in windows if w['frame'] in frames]
    if kinds: windows=[w for w in windows if w['name'] in kinds]
    index = IntervalIndex(windows, lambda w:w['begin'], lambda w:w['end'])
    matched=[]
    for row in samples:
        # SpinOSは1/60µs、デバッガは生QPC。整数変換する。
        begin=row['begin']*60_000_000//meta['hz']; end=row['end']*60_000_000//meta['hz']
        overlaps=list(index.overlap(begin,end))
        if not overlaps: continue
        hit=dict(row, module=resolve(modules,row['eip'],row['captured']),
                 pause_upper_us=(row['end']-row['begin'])*1_000_000/meta['hz'],
                 windows=[dict(frame=w['frame'],name=w['name'],duration_us=(w['end']-w['begin'])/60,
                    context_inside=w['begin'] <= row['captured']*60_000_000//meta['hz'] < w['end'],
                    sample_overlap_us=max(0,min(end,w['end'])-max(begin,w['begin']))/60) for w in overlaps])
        stack=bytes.fromhex(row['stack']); candidates=[]
        for offset in range(0,len(stack)-3,4):
            address=struct.unpack_from('<I',stack,offset)[0]; module=resolve(modules,address,row['captured'])
            if module: candidates.append(dict(stack_offset=offset,address=address,module=module))
        hit['stack_address_candidates']=candidates
        hit['ebp_chain']=frame_chain(row,modules)
        matched.append(hit)
    counts=Counter((x['module']['path'],x['module']['rva']) for x in matched if x['module'])
    intervals=[(b['begin']-a['begin'])*1_000_000/meta['hz'] for a,b in zip(samples,samples[1:])]
    pauses=[(s['end']-s['begin'])*1_000_000/meta['hz'] for s in samples]
    def stats(values):
        return dict(count=len(values),median=statistics.median(values),maximum=max(values)) if values else {}
    owners=[]
    for row in load_owners(debug,modules):
        at=row['main_captured']*60_000_000//meta['hz']
        overlapping=[w for w in index.overlap(at,at+1) if w['begin']<=at<w['end']]
        if overlapping:
            row['windows']=overlapping
            row['pause_upper_us']=(row['end']-row['begin'])*1_000_000/meta['hz']
            owners.append(row)
    return dict(source=str(debug),game=str(game),meta=meta,complete=ended,errors=errors,modules=modules,
                actual_sample_interval_us=stats(intervals),sample_pause_upper_us=stats(pauses),
                sample_count=len(samples),spike_window_count=len(windows),matched_samples=matched,
                candidates=[dict(path=p,rva=r,count=n) for (p,r),n in counts.most_common()],
                matched_owners=owners,
                debugger_events=events,
                explanation="採取でゲームスレッドを停止するため重なりは原因の証明ではない。pause_upper_usはSuspend要求からResume復帰までの上限。スタック値は戻り先と保証しない。2ms級採取は短いスパイクを取り逃す。")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pair',type=Path)
    parser.add_argument('--minimum-us',type=float,default=100)
    parser.add_argument('--frame',type=int,action='append',help='このフレームだけ。複数指定可能')
    parser.add_argument('--kind',action='append',help='textureTransfer / drawIndexed 等の区間名')
    parser.add_argument('--output',type=Path,help='新規出力ディレクトリ（既存結果は上書きしない）')
    parser.add_argument('--objdump',type=Path,default=Path('C:/msys64/mingw32/bin/objdump.exe'))
    args=parser.parse_args()
    if args.minimum_us <= 0: parser.error('minimum-us must be positive')
    output=args.output or args.pair/'spike_debug_analysis'; output.mkdir(exist_ok=False)
    reports=[]
    for side in (1,2):
        for debug in sorted(args.pair.glob(f'game_{side}_spike_debug_*.tsv')):
            report=analyze(debug,args.pair/f'game_{side}.log',args.minimum_us,args.frame,args.kind)
            selected=[]; seen=set()
            for sample in sorted(report['matched_samples'],key=lambda x:max(w['duration_us'] for w in x['windows']),reverse=True):
                key=(sample['eip'],sample['code'])
                if key in seen or not sample['code']: continue
                seen.add(key); selected.append(sample)
                if len(selected)==40: break
            for i,sample in enumerate(selected):
                stem=f"{debug.stem}_{i:02d}_{sample['eip']:08x}"
                binary=output/(stem+'.bin'); binary.write_bytes(bytes.fromhex(sample['code']))
                sample['live_bytes_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
                if args.objdump.is_file():
                    result=subprocess.run([str(args.objdump),'-D','-b','binary','-m','i386','-M','intel',f"--adjust-vma={sample['eip']}",str(binary)],capture_output=True,text=True)
                    (output/(stem+'.asm')).write_text(result.stdout+result.stderr,encoding='utf-8')
                    sample['disassembly']=stem+'.asm'; sample['objdump_exit']=result.returncode
            for i,sample in enumerate(report['matched_owners'][:40]):
                if not sample['owner_stable'] or not sample['code'] or sample['error']: continue
                stem=f"{debug.stem}_owner_{i:02d}_{sample['eip']:08x}"
                binary=output/(stem+'.bin');binary.write_bytes(bytes.fromhex(sample['code']))
                if args.objdump.is_file():
                    result=subprocess.run([str(args.objdump),'-D','-b','binary','-m','i386','-M','intel',f"--adjust-vma={sample['eip']}",str(binary)],capture_output=True,text=True)
                    (output/(stem+'.asm')).write_text(result.stdout+result.stderr,encoding='utf-8')
                    sample['disassembly']=stem+'.asm';sample['objdump_exit']=result.returncode
            reports.append(report)
    (output/'report.json').write_text(json.dumps(reports,ensure_ascii=False,indent=2),encoding='utf-8')
    lines=['# スパイク箇所の調査候補','', '採取で実行が停止する診断モード。頻度や時間を通常測定として扱わない。スタック候補は確定した呼出履歴ではない。','']
    for r in reports:
        lines += [f"## PID {r['meta']['pid']}",f"採取 {r['sample_count']} 件、閾値以上の区間 {r['spike_window_count']} 件、重複採取 {len(r['matched_samples'])} 件、正常終端 {r['complete']}。",'', '| モジュール | RVA | 採取件数 |','|---|---:|---:|']
        lines += [f"| {x['path']} | 0x{x['rva']:X} | {x['count']} |" for x in r['candidates'][:20]]
        lines += ['', '詳細なフレーム・レジスタ・採取停止時間・命令バイトはreport.json。*.asmは採取時EIPからの実メモリ命令。分岐後の表・データも直線的に解読されるため、全行が実行命令とは限らない。','']
        lines += [f"実採取間隔 µs: {r['actual_sample_interval_us']}",f"採取停止の上限 µs: {r['sample_pause_upper_us']}", '', '| フレーム・区間 | EIP | 命令列 |','|---|---:|---|']
        for sample in sorted(r['matched_samples'],key=lambda x:max(w['duration_us'] for w in x['windows']),reverse=True):
            if 'disassembly' not in sample: continue
            w=max(sample['windows'],key=lambda w:w['duration_us'])
            lines.append(f"| {w['frame']} / {w['name']} ({w['duration_us']:.1f}µs) | 0x{sample['eip']:X} | [実メモリ命令]({sample['disassembly']}) |")
        lines.append('')
        lines += ['### クリティカルセクション所有者','', '所有者スレッド停止後に同じロックの所有権を再確認した標本だけに命令列リンクを付ける。OS版の診断点が一致しない場合は採取しない。','']
        for sample in r['matched_owners']:
            if 'disassembly' in sample:
                lines.append(f"- TID {sample['tid']} / EIP 0x{sample['eip']:X}: [実メモリ命令]({sample['disassembly']})、採取停止上限 {sample['pause_upper_us']:.1f}µs")
        lines.append('')
    (output/'README.md').write_text('\n'.join(lines),encoding='utf-8')
    print(json.dumps(dict(output=str(output),processes=len(reports),matched=[len(r['matched_samples']) for r in reports])))
    if not reports: raise SystemExit('デバッグ採取ファイルがありません')

if __name__=='__main__': main()
