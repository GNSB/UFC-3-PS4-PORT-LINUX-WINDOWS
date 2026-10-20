import os
import sys, json, collections
from pathlib import Path
sys.path.insert(0,'scripts')
import capstone
from link_modules import module
G=Path(os.environ['GAME'])
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
p=json.load(open(os.environ['PLAN']))
SUP={'andn','bextr','blsr','blsi','blsmsk','tzcnt','lzcnt','movbe'}
def sweep(code, va):
    pos=0; found=[]
    while pos<len(code):
        n=0
        for a,sz,mn,op in md.disasm_lite(code[pos:], va+pos):
            n+=1
            if mn in SUP: found.append((a,sz,mn,op))
            pos=a-va+sz
        if pos<len(code): pos+=1
    return found
for name,path in (('eboot.bin',G/'eboot.bin'),('libc.prx',G/'sce_module/libc.prx'),('libSceFios2.prx',G/'sce_module/libSceFios2.prx')):
    m=module(path)
    sites=[x for x in p['sites'] if x['module']==name]
    # every BMI address inside a site range counts as covered
    rng=[(x['address'], x['address']+len(x['bytes'])//2) for x in sites]
    starts=sorted(rng)
    import bisect
    def inside(a):
        i=bisect.bisect_right(starts,(a,1<<62))-1
        return i>=0 and starts[i][0]<=a<starts[i][1]
    found=[]
    for seg in m['ph']:
        if seg['type']==1 and seg['flags']&1:
            found+=sweep(bytes(m['elf'][seg['offset']:seg['offset']+seg['filesz']]), seg['vaddr'])
    miss=[f for f in found if not inside(f[0])]
    print(name,'sweep BMI',len(found),'outside any site',len(miss), collections.Counter(f[2] for f in miss))
    for f in miss[:12]: print('   ',hex(f[0]),f[1],f[2],f[3])
