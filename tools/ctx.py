import sys
sys.path.insert(0, 'tools')
from disx import D, off, md, BASE, func_start
def ctx(r, before=0x40):
    s = func_start(r)
    print(f"== ret exe+{r:X}  func~exe+{s:X} (+{r-s:X})")
    # disassemble from function start to find correct alignment, print last lines
    code = D[off(s):off(r)+8]
    lines = [(i.address-BASE, i.mnemonic, i.op_str) for i in md.disasm(code, BASE+s)]
    idx = [k for k,(a,m,o) in enumerate(lines) if a < r]
    for a,m,o in lines[max(0, idx[-1]-14): idx[-1]+2]:
        print(f"  {a:08X}: {m} {o}{'  <== call' if a == lines[idx[-1]][0] else ''}")
for a in sys.argv[1:]:
    ctx(int(a,16))
