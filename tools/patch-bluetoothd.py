#!/usr/bin/env python3
"""bluetoothd of tvOS 16.5 (20L563) with every pairing agent programmatic: patch-bluetoothd.py <in> <out>.

In both handlers of CBMsgIdPairingAgentRegistered (0x1001ad7cc; 0x1003de024, a central session's, which also wants
the entitlement com.apple.BTServer.programmaticPairing, and TVSettings has not got it), `cset w20, ne` (programmatic)
becomes `mov w20, #1`; the agent's mode is then 2, and bluetoothd sends a pairing request to the agent's session
(CBMsgIdPairingAgentPairingRequested) instead of putting up its own alert, and takes the session's answer.
"""
import struct, sys

BEFORE, AFTER = 0x1a9f07f4, 0x52800034
data = bytearray(open(sys.argv[1], 'rb').read())
for address in 0x1001ad7cc, 0x1003de024:
    offset = address - 0x100000000          # __TEXT: vmaddr 0x100000000 at file offset 0
    found = struct.unpack_from('<I', data, offset)[0]
    if found != BEFORE:
        sys.exit(f'patch-bluetoothd: {found:#010x} at {address:#x}, expected {BEFORE:#010x}: another bluetoothd?')
    struct.pack_into('<I', data, offset, AFTER)
open(sys.argv[2], 'wb').write(data)
