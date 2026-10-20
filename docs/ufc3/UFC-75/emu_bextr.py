import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'emu.py')).read().split('bad=collections')[0])
import capstone
from capstone.x86 import X86_OP_REG, X86_OP_MEM
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail=True
REGIDX={n:i for i,n in enumerate(GN)}
def regval(name,regs):
    import re
    c={'eax':'rax','ebx':'rbx','ecx':'rcx','edx':'rdx','esi':'rsi','edi':'rdi','ebp':'rbp'}.get(name,name)
    c=re.sub(r'^(r\d+)[dwb]$',r'\1',c)
    return regs[REGIDX[c]]
def memread(addr,size):
    out=b''
    for a in range(addr,addr+size): out+=page(a&~0xfff)[a&0xfff:(a&0xfff)+1]
    return int.from_bytes(out,'little')
def ref(src,ctl,w):
    st=ctl&255; ln=(ctl>>8)&255; src&=(1<<w)-1
    if st>=w: return 0
    v=src>>st
    if ln<w: v&=(1<<ln)-1
    return v
rnd=random.Random(2); bad=0; n=0; ex=[]
for s in p['sites']:
    if not s['instruction'].startswith('bextr'): continue
    raw=bytes.fromhex(s['bytes'])
    insn=next(md.disasm(raw,0))
    if insn.mnemonic!='bextr': insn=list(md.disasm(raw,0))[1]
    d,src,ctl=insn.operands
    w=d.size*8
    for t in range(10):
        regs=[rnd.getrandbits(64) for _ in GPR] if src.type==X86_OP_REG else [rnd.randrange(0x10000,0x40000000)&~7 for _ in GPR]
        if t>=5: 
            for i in range(15): regs[i]=(regs[i]&~0xffff)|rnd.choice([0,1,8,31,32,63,64,200])|(rnd.choice([0,1,5,31,32,33,64,255])<<8) if src.type==X86_OP_REG else regs[i]
        b=run((bytes.fromhex(s['body']),True),regs,0x202)
        if b[0] in ('fault','rip'): continue
        if src.type==X86_OP_REG: sv=regval(insn.reg_name(src.reg),regs)
        else:
            m=src.mem; addr=m.disp
            if m.base: addr+=regval(insn.reg_name(m.base),regs) if insn.reg_name(m.base)!='rsp' else RSP
            if m.index: addr+=regval(insn.reg_name(m.index),regs)*m.scale
            sv=memread(addr&((1<<64)-1),d.size)
        cv=regval(insn.reg_name(ctl.reg),regs)
        exp=ref(sv,cv,w)
        dn=insn.reg_name(d.reg); got=regval(dn,b[0])
        if w==32: got&=0xffffffffffffffff
        n+=1
        if got!=exp:
            bad+=1
            if len(ex)<5: ex.append((s['module'],hex(s['address']),s['instruction'],hex(sv),hex(cv),hex(exp),hex(got)))
print('bextr checks',n,'mismatches vs reference',bad); print(*ex,sep='\n')
