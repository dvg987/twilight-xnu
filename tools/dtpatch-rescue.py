#!/usr/bin/env python3
"""Mark the DTS as ours and keep the XNU log window (0x07500000, 11 MiB) out of Linux's hands."""
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('\tmodel = "Xiaomi 3rd Gen";', '\tmodel = "Xiaomi 3rd Gen";\n\ttwilight-rescue = "1";', 1)
anchor = '\treserved-memory {\n\t\t#address-cells = <0x02>;\n\t\t#size-cells = <0x02>;\n\t\tranges;\n'
assert anchor in s
s = s.replace(anchor, anchor + '\n\t\txnulog@7500000 {\n\t\t\treg = <0x00 0x7500000 0x00 0xb00000>;\n\t\t\tno-map;\n\t\t};\n', 1)
open(p, 'w').write(s)
