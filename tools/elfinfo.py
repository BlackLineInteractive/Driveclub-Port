import struct, sys, collections
d=open(sys.argv[1],'rb').read()
n=struct.unpack_from('<H',d,24)[0]
segs={}
for i in range(n):
    p,o,s,m=struct.unpack_from('<QQQQ',d,32+i*32)
    if p&0x800: segs[(p>>20)&0xfff]=(o,s)
E=32+n*32
phoff,=struct.unpack_from('<Q',d,E+0x20); phnum,=struct.unpack_from('<H',d,E+0x38)
entry,=struct.unpack_from('<Q',d,E+0x18)
print('entry',hex(entry))
ph=[]
for i in range(phnum):
    t,f,o,va,pa,fs,ms,al=struct.unpack_from('<IIQQQQQQ',d,E+phoff+i*56)
    ph.append((t,f,o,va,fs,ms)); print(i,hex(t),'flags',f,'va',hex(va),'filesz',hex(fs),'memsz',hex(ms))
def seg(t):
    for i,p in enumerate(ph):
        if p[0]==t: o,s=segs[i]; return d[o:o+s],p
dynlib,pd=seg(0x61000000)
pdyn=[p for p in ph if p[0]==2][0]
dyn=dynlib[pdyn[2]-pd[2]:pdyn[2]-pd[2]+pdyn[4]]
tags=collections.defaultdict(list)
for i in range(0,len(dyn),16):
    t,v=struct.unpack_from('<qQ',dyn,i); tags[t].append(v)
strtab=tags[0x61000035][0]; strsz=tags[0x61000037][0]
def s(off): e=dynlib.index(b'\0',strtab+off); return dynlib[strtab+off:e].decode()
mods={}
for v in tags[0x6100000F]: mods[v>>48]=s(v&0xffffffff)
libs={}
for v in tags[0x61000015]: libs[v>>48]=s(v&0xffffffff)
print('NEEDED', [s(v) for v in tags[1]])
symtab=tags[0x61000039][0]; symsz=tags[0x6100003f][0]
cnt=collections.Counter()
import base64
alpha='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
for i in range(0,symsz,24):
    nm,info,oth,shn,val,sz=struct.unpack_from('<IBBHQQ',dynlib,symtab+i)
    name=s(nm)
    if '#' in name and shn==0:
        _,l,m=name.split('#'); li=0
        for c in l: li=li*64+alpha.index(c)
        cnt[libs.get(li,l)]+=1
print('IMPORTS per library:')
for k,v in cnt.most_common(): print(f'  {v:5d} {k}')
print('total',sum(cnt.values()))
if len(sys.argv)>2:
    for i in range(0,symsz,24):
        nm,info,oth,shn,val,sz=struct.unpack_from('<IBBHQQ',dynlib,symtab+i)
        name=s(nm)
        if '#' in name and shn==0:
            n,l,m=name.split('#'); li=0
            for c in l: li=li*64+alpha.index(c)
            if libs.get(li)==sys.argv[2]: print('NID',n)
