"""pe-fixup.py — post-process objcopy's efi-app output so a strict EDK2/UEFI
loader accepts the image.

objcopy sets IMAGE_FILE_RELOCS_STRIPPED because the image carries no .reloc
section. Combined with ImageBase=0 that tells the firmware "load me at address 0
and do not move me", which no loader can honour — EDK2 then refuses the image.
vboot is built -fpic with zero relocations, so it runs correctly wherever the
firmware places it; clearing the flag is all that is needed. (The distro's
systemd-bootaa64.efi has exactly the same shape: no .reloc, RELOCS_STRIPPED
clear.)

Usage: python3 pe-fixup.py <file.efi>
"""
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
pe = data.find(b"PE\x00\x00")
if pe < 0:
    sys.exit("pe-fixup: not a PE image: %s" % path)
off = pe + 4
chars, = struct.unpack_from("<H", data, off + 18)
fixed = chars & ~0x0001                      # clear IMAGE_FILE_RELOCS_STRIPPED
struct.pack_into("<H", data, off + 18, fixed)
open(path, "wb").write(data)
print("pe-fixup: characteristics 0x%04x -> 0x%04x" % (chars, fixed))
