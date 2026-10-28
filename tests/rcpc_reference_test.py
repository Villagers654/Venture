# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble authored RCpc forms and check the JIT against an independent model."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

p=argparse.ArgumentParser()
for name in ('clang','objcopy','driver'):
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--expect-undefined',action='store_true')
args=p.parse_args()
started=time.perf_counter()
with tempfile.TemporaryDirectory(prefix='rcpc-reference-') as temporary:
    root=Path(temporary);rows=[]
    def add(assembly,**fields):
        rows.append(dict(name=assembly,**fields))
    for width,op,reg in ((8,'ldaprb','w'),(16,'ldaprh','w'),(32,'ldapr','w'),(64,'ldapr','x')):
        for sp in (False,True):
            for rt in (2,31):
                add(f'{op} {reg}{rt}, [{"sp" if sp else "x0"}]',width=width,load=True,pair=False,writeback=False,sp=sp,rt=rt)
    for width,reg in ((32,'w'),(64,'x')):
        for load,op in ((True,'ldiapp'),(False,'stilp')):
            for sp in (False,True):
                for writeback in (False,True):
                    base='sp' if sp else 'x0';size=width//4
                    address=f'[{base}], #{size}' if load and writeback else f'[{base}, #-{size}]!' if writeback else f'[{base}]'
                    for rt,rt2 in ((2,3),(31,3),(2,31)):
                        add(f'{op} {reg}{rt}, {reg}{rt2}, {address}',width=width,load=load,pair=True,writeback=writeback,sp=sp,rt=rt,rt2=rt2)
    # Assembler warns on constrained-unpredictable overlaps; the frontend's
    # chosen permitted behavior is a trap. This is separate from normal cases.
    for reg,width in (('w',32),('x',64)):
        add(f'ldiapp {reg}2, {reg}2, [x0]',width=width,load=True,pair=True,writeback=False,sp=False,rt=2,rt2=2,trap=True)
    # Every authored A64 case is one fixed-width instruction. Assemble the
    # entire fixture once, then split its text bytes without decoding them.
    # This preserves the independent descriptor model and all random states.
    source=root/'cases.S';obj=root/'cases.o';code=root/'cases.bin'
    source.write_text('.text\n'+'\n'.join(row['name'] for row in rows)+'\n')
    subprocess.run([str(args.clang),'-target','aarch64-none-elf','-march=armv8.9-a+rcpc3','-c',str(source),'-o',str(obj)],check=True,capture_output=True)
    subprocess.run([str(args.objcopy),'-O','binary','--only-section=.text',str(obj),str(code)],check=True)
    words=code.read_bytes()
    if len(words)!=4*len(rows):
        raise AssertionError('authored RCpc cases must each assemble to exactly four bytes')
    for index,row in enumerate(rows):
        path=root/(str(index)+'.bin')
        path.write_bytes(words[4*index:4*index+4])
        row['code']=str(path)
    print(f'Assembled {len(rows)} authored cases in {time.perf_counter()-started:.3f}s',flush=True)
    manifest=root/'cases.json';manifest.write_text(json.dumps(rows))
    result=subprocess.run([str(args.driver),str(manifest)]+(['--expect-undefined'] if args.expect_undefined else []),capture_output=True,text=True)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    result.check_returncode()
    evidence=json.loads(result.stdout)
    if evidence['matched'] is not True or evidence['checked']!=len(rows)*2*256:
        raise AssertionError('reference driver did not validate the complete case/state matrix')
    expected_traps=sum(args.expect_undefined or row.get('trap',False) for row in rows)*2*256
    if evidence['overlap_traps']!=expected_traps:
        raise AssertionError('reference driver did not validate the complete trap matrix')
    print(f'Full reference matrix completed in {time.perf_counter()-started:.3f}s',flush=True)
