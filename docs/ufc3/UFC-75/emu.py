import os
import json, random, hashlib, collections, sys
from unicorn import *
from unicorn.x86_const import *
p=json.load(open(os.environ['PLAN']))
GPR=[UC_X86_REG_RAX,UC_X86_REG_RBX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_RSI,UC_X86_REG_RDI,UC_X86_REG_RBP,
     UC_X86_REG_R8,UC_X86_REG_R9,UC_X86_REG_R10,UC_X86_REG_R11,UC_X86_REG_R12,UC_X86_REG_R13,UC_X86_REG_R14,UC_X86_REG_R15]
GN='rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15'.split()
CF,ZF,SF,OF=1,1<<6,1<<7,1<<11
MASK={'andn':CF|ZF|SF|OF,'bextr':CF|ZF|OF,'blsi':CF|ZF|SF|OF,'blsr':CF|ZF|SF|OF,'blsmsk':CF|ZF|SF|OF,
      'tzcnt':CF|ZF,'lzcnt':CF|ZF,'movbe':0xcd5}
CODE=0x400000000; TRAMP=0x500000000; RSP=0x7ff00000
def page(addr):
    h=hashlib.blake2b(addr.to_bytes(8,'little')).digest()
    rnd=random.Random(h); b=bytearray(rnd.randbytes(4096))
    for i in range(0,4096,64): b[i:i+8]=bytes(8)  # some zero qwords
    return bytes(b)
def run(code, regs, flags):
    mu=Uc(UC_ARCH_X86, UC_MODE_64); mu.ctl_set_cpu_model(UC_CPU_X86_HASWELL)
    mapped=set(); writes=set()
    def mp(a):
        a&=~0xfff
        if a in mapped: return
        mu.mem_map(a,0x1000); mu.mem_write(a,page(a)); mapped.add(a)
    def unm(uc,acc,addr,size,val,ud):
        if not (0<=addr<(1<<47)): return False
        mp(addr); mp(addr+size-1); return True
    def wr(uc,acc,addr,size,val,ud): writes.update(range(addr,addr+size))
    mu.hook_add(UC_HOOK_MEM_UNMAPPED, unm); mu.hook_add(UC_HOOK_MEM_WRITE, wr)
    base=TRAMP if code[1] else CODE
    mu.mem_map(base, 0x10000); mu.mem_write(base, code[0])
    for r,v in zip(GPR,regs): mu.reg_write(r,v)
    mu.reg_write(UC_X86_REG_RSP,RSP); mu.reg_write(UC_X86_REG_EFLAGS,flags)
    try: mu.emu_start(base, base+len(code[0]), count=500)
    except UcError as e: return ('fault',str(e))
    if mu.reg_read(UC_X86_REG_RIP)!=base+len(code[0]): return ('rip',hex(mu.reg_read(UC_X86_REG_RIP)))
    mem={a:bytes(mu.mem_read(a,1)) for a in writes if not (RSP-128-256<=a<RSP-128)}
    return ([mu.reg_read(r) for r in GPR], mu.reg_read(UC_X86_REG_RSP), mu.reg_read(UC_X86_REG_EFLAGS), mem)
bad=collections.Counter(); ex={}
rnd=random.Random(1)
sites=p['sites']
for n,s in enumerate(sites):
    mn=s['instruction'].split()[0]
    orig=bytes.fromhex(s['patch']) if 'patch' in s else bytes.fromhex(s['bytes'])
    if 'patch' in s: tr=(bytes.fromhex(s['patch']),True)
    else: tr=(bytes.fromhex(s['body']),True)
    hasmem='[' in s['instruction']
    for t in range(8):
        if t==0: regs=[0]*15
        elif t<4 and not hasmem: regs=[rnd.getrandbits(64) for _ in GPR]
        else: regs=[rnd.randrange(0x10000,0x40000000) & ~7 for _ in GPR]
        if t in (5,6):  # zero one random register (source operands)
            regs[rnd.randrange(15)]=0
        if t==7: regs=[1<<rnd.randrange(31) for _ in GPR]
        fl=0x202|(rnd.getrandbits(1)*CF)|(rnd.getrandbits(1)*ZF)
        a=run((bytes.fromhex(s['bytes']) if 'patch' not in s else bytes.fromhex(s['bytes']),False),regs,fl)
        b=run(tr,regs,fl)
        if a[0] in('fault','rip') or b[0] in ('fault','rip'):
            if a!=b: bad[(mn,'fault')]+=1; ex.setdefault((mn,'fault'),(s['module'],hex(s['address']),s['instruction'],a,b))
            continue
        k=None
        if a[0]!=b[0]: k='regs'
        elif a[1]!=b[1]: k='rsp'
        elif (a[2]^b[2])&MASK[mn]: k='flags'
        elif a[3]!=b[3]: k='mem'
        if k:
            bad[(mn,k)]+=1
            if (mn,k) not in ex:
                d=[(GN[i],hex(x),hex(y)) for i,(x,y) in enumerate(zip(a[0],b[0])) if x!=y] if k=='regs' else (hex(a[2]),hex(b[2])) if k=='flags' else ''
                ex[(mn,k)]=(s['module'],hex(s['address']),s['instruction'],[hex(r) for r in regs][:6],d)
    if n%1000==0: print(n, dict(bad), file=sys.stderr, flush=True)
print('TOTAL sites', len(sites)); print(dict(bad))
for k,v in ex.items(): print(k, v)
