import tempfile
import unittest
from pathlib import Path
from analyze_spike_debug import analyze, load_debug, resolve, frame_chain, load_owners
import struct


class DebugAnalysis(unittest.TestCase):
    def test_owner_requires_explicit_stability_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'debug.tsv'
            prefix='OWNER\t100\t4096\t42\t0\t1\t101\t102\t103\t0\t0\t8192\t1000\t0\t90\t'
            path.write_text(prefix+'\n'+prefix+'\t0\n'+prefix+'\t1\n')
            rows=load_owners(path,[])
            self.assertEqual([r['owner_stable'] for r in rows],[False,True])

    def test_bounded_ebp_chain_rejects_cycle(self):
        row=dict(esp=1000,ebp=1000,captured=10,stack=struct.pack('<IIII',1008,4100,1000,4104).hex())
        modules=[dict(begin=1,end=None,base=4096,size=256,path='a.dll',stamp=1)]
        chain=frame_chain(row,modules)
        self.assertEqual([x['module']['rva'] for x in chain],[4,8])
        row['ebp']=999
        self.assertEqual(frame_chain(row,modules),[])

    def test_texture_and_draw_windows_and_frame_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);debug=p/'d.tsv';game=p/'g.log'
            debug.write_text('META\t1\t10\t20\t10000000\t200\t200000\n'
                'SAMPLE\t100\t110\t120\t0\t4100\t8000\t0\t0\t0\t0\t0\t0\t0\t0\t90c3\t\nEND\t200\t1\n')
            game.write_text('[TextureTransfer] f=660 begin=600 end=900 pid=10 tid=20\n'
                '[DrawWork] f=661 begin=600 end=900 pid=10 tid=20\n')
            result=analyze(debug,game,.1,[660],['textureTransfer'])
            self.assertEqual(result['spike_window_count'],1)
            self.assertTrue(result['matched_samples'][0]['windows'][0]['context_inside'])
    def test_module_address_reuse(self):
        modules=[dict(begin=1,end=10,base=4096,size=256,path='old',stamp=1),
                 dict(begin=20,end=None,base=4096,size=256,path='new',stamp=2)]
        self.assertEqual(resolve(modules,4100,5)['path'],'old')
        self.assertIsNone(resolve(modules,4100,15))
        self.assertEqual(resolve(modules,4100,25)['rva'],4)
        self.assertIsNone(resolve(modules,4352,25))

    def test_qpc_conversion_pid_filter_and_stack_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory); debug=p/'debug.tsv'; game=p/'game.log'
            debug.write_text('META\t1\t10\t20\t10000000\t2000\t200000\n'
                'MODULE\t1\t4096\t256\t123\tgame.exe\n'
                'SAMPLE\t100\t110\t120\t0\t4100\t8000\t8004\t0\t0\t0\t0\t0\t0\t0\t90c3\t08100000\n'
                'END\t200\t1\n',encoding='utf-8')
            game.write_text('[ReplayWork] f=100 begin=600 restoreEnd=650 end=900 pid=10 tid=20\n'
                            '[ReplayWork] f=999 begin=600 restoreEnd=650 end=900 pid=11 tid=20\n',encoding='utf-8')
            result=analyze(debug,game,.1)
            sample=result['matched_samples'][0]
            self.assertTrue(result['complete'])
            self.assertEqual(sample['module']['rva'],4)
            self.assertEqual(sample['pause_upper_us'],2)
            self.assertEqual({w['frame'] for w in sample['windows']},{100})
            self.assertFalse(next(w for w in sample['windows'] if w['name']=='replayRestore')['context_inside'])
            self.assertTrue(next(w for w in sample['windows'] if w['name']=='replayExecution')['context_inside'])
            self.assertEqual(sample['stack_address_candidates'][0]['module']['rva'],8)

    def test_truncated_tail_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'debug.tsv'
            p.write_text('META\t1\t10\t20\t10000000\t2000\t200000\nSAMPLE\t123',encoding='utf-8')
            _,_,samples,_,errors,ended=load_debug(p)
            self.assertEqual(samples,[])
            self.assertFalse(ended)
            self.assertTrue(errors)

if __name__=='__main__': unittest.main()
