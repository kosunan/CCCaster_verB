"""Present間隔とPresentMonの表示推定値を比較する。複数DWM出力の混在に注意。"""
import argparse
from bisect import bisect_left
import csv
import json
from pathlib import Path
import struct

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def series(directory):
    pid = json.loads((directory/'capture.json').read_text(encoding='utf-8'))['game_pid']
    raw = (directory/'frames.bin').read_bytes()
    records = list(struct.iter_unpack('<qq8I',raw[4096:]))
    ticks = [r[0] for r in records]
    values = [[],[]]
    with (directory/'presents.csv').open(encoding='utf-8-sig',newline='') as file:
        for row in csv.DictReader(file):
            if int(row['ProcessID']) != pid:
                continue
            i=bisect_left(ticks,int(row['QPCTime']))
            if i<1 or i>=len(records) or any(records[n][3]!=1 or records[n][7]!=0 for n in (i-1,i)):
                continue
            values[0].append(float(row['msBetweenPresents']))
            if row['Dropped']=='0' and float(row['msBetweenDisplayChange'])>0:
                values[1].append(float(row['msBetweenDisplayChange']))
    return values


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories',nargs='+',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    fig,axes=plt.subplots(1,2,figsize=(11,4.4),layout='constrained')
    for directory in args.directories:
        for axis,values in zip(axes,series(directory)):
            values.sort()
            axis.plot(values,[100*(n+1)/len(values) for n in range(len(values))],
                      label=directory.name.replace('_',' '),linewidth=1.7)
    for axis,title in zip(axes,['Application Present interval','PresentMon display estimate (check output attribution)']):
        axis.axvline(1000/60,color='#777777',linestyle='--',linewidth=1,label='60 Hz period')
        axis.set(xlabel='Interval (ms)',ylabel='Cumulative frames (%)',title=title,xlim=(7,30),ylim=(0,101))
        axis.grid(alpha=.2); axis.legend(loc='lower right',fontsize=8)
    fig.suptitle('MBAACC replay: windowed vs. borderless | 2560 x 1440, 120 Hz\nActive battle, including KO animation; intro excluded',fontsize=12)
    fig.savefig(args.output,dpi=160)
