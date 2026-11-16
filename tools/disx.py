import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
P = r"W:/SteamLibrary/steamapps/common/Borderlands 2/Binaries/Win32/Borderlands2.exe"
D = open(P,'rb').read()
BASE = 0x400000
def off(rva):  # .text raw 0x400 at va 0x1000; .rdata raw 0x1215600 at 0x1217000
    if rva < 0x1217000: return rva - 0x1000 + 0x400
    if rva < 0x15b8000: return rva - 0x1217000 + 0x1215600
    return rva - 0x15b8000 + 0x15b6200
def byte(rva): return D[off(rva)]
def func_start(rva):
    r = rva
    while r > 0x1000:
        r -= 1
        if r % 16 == 0 and byte(r-1) in (0xCC, 0xC3, 0x90) and byte(r) in (0x55, 0x83, 0x6A, 0x56, 0x53, 0x8B, 0x51, 0x81, 0x64, 0xA1, 0x57, 0xB8, 0xD9, 0x33, 0x68, 0xE9):
            # require 0xCC padding run or ret just before
            return r
    return None
md = Cs(CS_ARCH_X86, CS_MODE_32)
def dis(rva, n=0x200, mark=None):
    code = D[off(rva):off(rva)+n]
    for ins in md.disasm(code, BASE+rva):
        m = ' <==' if mark and ins.address + ins.size == BASE+mark else ''
        print(f"  {ins.address-BASE:08X}: {ins.mnemonic} {ins.op_str}{m}")
        if ins.mnemonic == 'int3': break
if __name__ == '__main__':
    for a in sys.argv[1:]:
        if a.startswith('f:'):
            r = int(a[2:],16); s = func_start(r)
            print(f"== ret exe+{r:X} in func exe+{s:X} (+{r-s:X})")
            dis(s, min(r-s+0x10, 0x1800), mark=r)
        else:
            r = int(a,16); print(f"== exe+{r:X}"); dis(r)
