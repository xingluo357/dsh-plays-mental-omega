import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# 用法: python disasm.py <gamemd.exe> <VA> [字节数]
# 例:   python disasm.py "C:\path\to\gamemd.exe" 0x4fa350 120
path = sys.argv[1]
data = open(path, "rb").read()

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
opt = e_lfanew + 24
image_base = struct.unpack_from("<I", data, opt + 28)[0]
secs = []
so = opt + struct.unpack_from("<H", data, e_lfanew + 20)[0]
for i in range(nsec):
    off = so + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode("latin1")
    vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, va, vsize, rptr, rsize))

def foff(va):
    rva = va - image_base
    for name, sva, vsize, rptr, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            return rptr + (rva - sva)
    raise SystemExit("va not in any section: %x" % va)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

va = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 120
o = foff(va)
code = data[o:o + n]
print("image_base=%08x sections=%s" % (image_base, [(s[0], hex(s[1])) for s in secs]))
for ins in md.disasm(code, va):
    print("%06x  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
