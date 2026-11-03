# Amlogic W2 Bluetooth over USB

From Amlogic's drivers: bt-modules amlogic/usb_driver_bt/amlbt.c (S1, legacy layout) and CoreELEC's w2_bt.ko of
2025-09-18 (S3, disassembled; the layout used here, matching CoreELEC's w2_bt_fw_usb.bin). All BT access is the W2
command block (CBW) on bulk OUT **EP2** with data on bulk IN **0x82**: READ_REG 0xC4 (data_len 4, flag 0x80, then 4 B
IN), WRITE_REG 0xC5 (cdb2 = value, cdb3 = 4, no data), WRITE_SRAM 0xC8 (+ data OUT), READ_SRAM 0xC9 (flag 0x80, + data
IN). One lock for all of the bus (WiFi's EP1/EP4 too). Bring WiFi up first.

## Bring-up (w2_bt.ko amlbt_char_open, amlbt_load_firmware)
1. 0xf00060 (RG_AON_A24): set bit 24, clear bits 27 and 26.
2. 0xf0303c bits 3:0 = PMU state; if 7 or 8: 1 ms, then 0xf03040 = (v & ~3) | 2.
3. 0xf00044: every 20 ms until bit 29 clears (<= 51 tries).
4. 0xf03058 = 0x00050005, 1 ms, = 0x00040004 (BT CPU and PHY held in reset; never the MAC bit 1).
5. Shared memory init (below).
6. 0xf03050 = 0 (RAM power). Firmware file: u32 iccm_size (0x80000), u32 dccm_size (0x20000), then ICCM (first
   0x40000 = ROM, not sent), DCCM. Send file[8 + 0x40000, +0x40000) to 0x00340000 and file[8 + iccm_size, +0x20000) to
   0x00400000, in 4 KiB WRITE_SRAMs (Linux reads each back and compares).
7. Config (read-modify-write): 0xf06308 bits 1:0 = BtIsolation (0); 0xf03040 = (v & 0xEDFFFFFF) | antenna 2 << 28 |
   sink 0 << 25; 0xf000d4 = (v & 0xFF4F0000) | BrDigitGain 66 | EdrDigitGain 98 << 8 | pinmux 0 << 20 | factory 0 << 21
   | system 0 << 23; 0xf000ec bits 1:0 = fw log 0. WRITE_SRAM 48 B ManfData (zeros) to 0x514970.
8. 0xf000d0 |= 1; 0xf03058 = 0: the BT CPU runs. (0xf0303c bits 3:0 should read 6.) Proof: HCI_Reset answered.

## Shared memory (S3)
Status 0x5101fc: |= 2 while rewriting ring pointers, then 0. Init: zero every pointer below; WRITE_SRAM 0x80 B to
0x510018 with 8 entries {0xFFFFFFFF, 0, 0, 0}.
- Command ring: base 0x518000, size 0x1000; read ptr 0x510000 (firmware), write ptr 0x510004 (host).
- ACL TX: table 0x510018 + 16 i (i < 8) {u32 prio, u32 handle, u32 status (0 free, 1 used, 2 done), 0}; buffers
  0x508000 + 0x408 i.
- RX block: READ_SRAM 0x93c B at 0x514000: +0x00 type read (host), +0x04 event read (host), +0x08 0, +0x0c ACL read
  (host), +0x10 ACL write (fw), +0x14 active (fw), +0x20 type write (fw), +0x24 event write (fw), +0x3c type ring
  (0x100), +0x13c event ring (0x800). ACL RX ring: 0x500000, size 0x1020.
Pointers are byte offsets in their ring, wrapping; free = size - used - 4.

## HCI
No H4 type byte. Command: opcode lo, hi, plen, params, padded to 4; check free space against 0x510000; WRITE_SRAM at
0x518000 + w (split at the ring end); w = (w + L4) % 0x1000; WRITE_REG 0x510004 = w.
Poll: read the RX block; nothing new if type write == type read. Type ring: 4-byte entries (byte 0 = 2 or 4): move the
read pointer to the write pointer. Events at event ring: {0x04, code, plen, params, pad}, record (plen + 6) & ~3; the HCI
event is plen + 2 bytes from byte 1. ACL: if ACL write != read, READ_SRAM 0x500000 (0x1020 B); records {0x02, 3 pad,
u16 handle|flags, u16 N, payload N, pad}, record (N + 11) & ~3, N < 0x405; ACL packet = N + 4 bytes from byte 4.
Ack: WRITE_SRAM 16 B to 0x514000 {typeR & 0xff, evtR & 0x1fff, 0, aclR & 0x1fff}. Poll every 1 ms if active, else 8.
ACL out: read the table (0x80 B), entries with status 2 become free; first status-0 entry i; prio = 1 + highest prio of
that handle (0 if none); WRITE_SRAM the packet (handle/flags, length, payload; padded 4) to 0x508000 + 0x408 i, then
{prio, handle, 1, 0} to 0x510018 + 16 i.
No vendor HCI commands are needed. BD_ADDR: from the firmware (efuse words 2/3 via WiFi's MM_SUB_GET_EFUSE as a check).
