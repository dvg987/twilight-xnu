# Amlogic W2 over USB: transport, bring-up and boot messages

Condensed from Amlogic's driver (aml_drv/, common/ of android_kernel_amlogic_amlogic-drivers_w2; layouts compiled
from its headers with the USB defines). Register/SRAM access = the 512-byte CBW on bulk OUT EP1 (READ_REG 0xC4,
WRITE_REG 0xC5, WRITE_SRAM 0xC8, READ_SRAM 0xC9), data back on bulk IN EP4. AML_ADDR_SYSTEM addresses get +0x60000000;
IPC registers at 0x60800000 + 4 * index.

## Bring-up (aml_sdio_usb_platform_on, aml_platform.c:1103)
1. bbpll (if 0x00f0101c bit 31 clear). 2. write 0x00a0d090 = 0x4f530033; 0x00a0d080 |= 0x30000.
3. reset: write 0x608010E0 = 3; 0x00f02040 &= ~(3 << 30); 10 ms; a = read 0x608010E0 (bit 0 must be clear);
   write a & ~2; 10 ms.
4. mpif: sig = read 0x60801000; if (sig >> 16) not 0xCAFE/0xC0CA and (sig & 0xf) < 3: write 0x60C10030 = 3.
5. AGC: rf = (read 0x60C00000 >> 16) & 0xf (0 TRIDENT, 1 CATAXIA, 2 KARST; else fail). agc reg = 0x60C0B390
   (TRIDENT 0x60C02060); clk reg = 0x60C00874 (TRIDENT 0x60C00848; signature 0xC0CA: 0x60805010);
   ver = TRIDENT ? 0 : (read 0x60C0B000 >> 22) & 3. Stop: agc |= BIT12; clk: ver > 0 ? |= BIT29 : &= ~BIT3.
   Write agc_ram (aml_agcram.h, 512 words = 2048 B = common/agcram_ind_20230223.bin?) with one WRITE_SRAM to 0x60C0A000.
   ver == 1: write 0x60C0B008 = BIT28, wait for it to clear, require 0x60C0B238 bit 0. Restart: clk back
   (ver > 0: &= ~BIT29; else |= BIT3); agc &= ~BIT12.
6. firmware download, START_WIFI. 7. write 0x608010E0 = 0x10; 0x00f02058 = 0x70000; poll 0x00a10004 == 0x1b8e.
8. READ_SRAM 16 B at 0x60000000 (compatibility_tag): ipc_shared_size u16 9564, msg_api u16 33, version u8 11,
   msge2a 64, dbgbuf 32, radar 16, unsuprxvec 8, rxdesc 128, rxbuf 128, txq bk/be/vi/vo 64, bcn 8.
9. write 0x6080000C = 0x00004FBF (unmask E2A IRQs). 10. RX state reset (EXPAND layout, 21 TX pages). 11. poll IRQs.
aml_ipc_init writes nothing to the chip on USB.

## A2E messages
lmac_msg: id u16, dest u16, src u16 (100), param_len u16 (round up 4), params. Write 8 + param_len bytes (<= 508) with
WRITE_SRAM to 0x60000014 (two-stage: CBW then data; CBW bytes 510-511 = 0), then write register 0x60800000 = 2.
One message outstanding at a time: free again on istatus MSG_ACK (bit 2) or when its CFM came (either order).
Timeout 6 s.

## Events: EP0 vendor IN 0xC2/0xCD, wValue 0, wIndex = counter (0, then +1 each), wLength 16
Reply u32[4]: [0] RX head (fw rx ptr), [1] istatus, [2],[3] BT. Poll again after ~1 ms.
istatus bits: 0x0004 MSG_ACK; 0x0008 RXDESC; 0x0010 radar; 0x0020 extended IRQ (read register 0x00a10034:
1 TX_STOP, 2 TX_START, 4 LA, 7 exception); 0x0F80 TXCFM (per AC); 0x1000 debug string (READ_SRAM 260 B at
0x6000072C, then write 0x60800000 = 1); 0x2000 E2A message; 0x4000 trace; 0x8000 channel switch (READ_SRAM 8 B at
0x00a17fc0: u32 msg_id (0x44/0x45), chan_index, roc, vif_index, roc_tdls).
E2A message: READ_SRAM 1036 B at 0x60000210 and 1036 B at 0x60012cc8, write 0x60800000 = 0x10, then handle each that
has pattern (u32 @1032) == 0xADDEDE2A and id != 0. ipc_e2a_msg: id u16 @0, dest u16, src u16, param_len u16 @6,
param @8 (1024 B), pattern @1032.

## TX
One page per frame: txdesc_host (72 B, see w2-station.md) + payload (frame minus its 14-byte Ethernet header), padded to
4; page <= 4624 B. WRITE_PACKET CBW (overlaid, 512 B, else zero): bytes 0-11 = u16 size[0..5] of up to 25 pages;
byte 12 = page count N; byte 13 = size[13] high byte, byte 14 = size[13] low byte; byte 15 = 0xC7; byte 16 = 0;
17-18 size[12]; 19-30 size[6..11]; 31 + 2k: size[14 + k]. Then one bulk OUT on EP1 with all pages back to back.
Pages: 21 free at start (EXPAND), one kept in reserve; each confirmation returns one.
Host ids: 520 slots; slot i starts as ((i << 16) | i) & 0x3FFFFFF; after a confirmation ((++prefix << 16) |
(id & 0xFFFF)) & 0x3FFFFFF, prefix starting at 521.
TX confirmations (on any TXCFM bit): bulk IN 2048 B on EP5, no CBW: 256 x 8 B {u16 status (bit0 done, bit1 retry,
bit2 sw retry, bit3 acked, sn 4-15), u16 amsdu_size, u32 ampdu_size:6 | hostid:26}. Walk from the saved index,
stop at the first entry whose hostid is not outstanding.

## RX
Ring 0x6001B400 .. end 0x600684b0 (EXPAND; NARROW: 0x60029130). r = fw_ptrs[0]: 0 = not up; head = 0x60000000 |
(r & ~(BIT19|BIT20|BIT21)), | BIT31 if r & BIT19 (phase); BIT20 NARROW request, BIT21 EXPAND request.
Tail: first READ_SRAM 4 B at 0x60B081D0; empty when tail == head (with phase). Read head - tail (or end - tail when
wrapped), <= 128 KiB, with READ_SRAM (flag 0x80) + bulk IN on EP4. At the end (or < 128 B before it) the tail goes back
to 0x6001B400 with the head's phase.
Record at d: hw_rxhdr at d+48 (frmlen u16 @48, payl_offset @52, rx_status @53, rssi_leg s8 @62, rssi1 s8 @65,
next u32 @76 (absolute chip address of the next record), phy_info @88 (band, channel_type, prim20 u16 @90, center1,
center2), flags u32 @96 (bit0 amsdu, bit1 80211_mpdu, bits 4-6 TID, 8-15 vif, 16-23 sta)); payload p = d + 128
(+36 if frmlen != 0); if payl_offset >= 4, p starts with aml_rhd_ext {u16 sn:12 fn:4, u16 flags (pn_present bit15),
u64 pn}; frame at p + payl_offset, frmlen bytes. Record length = next - d (next == ring start: to the end).
Wrapping records (next < d, next != start), len = end - d: len < 164 -> first 128 B at d then [start, next);
mpdu or len < 220 -> first 164 B, then [start, next); else all of it, then [start + 36, next).
Skip: rx_status 0 or frmlen 0; drop ETH_LEN_UPDATE 0x10 / COPY 0x20 / MONITOR 0x80 / DELETE 4 unless the status is
exactly HOST_REO 0x6 or DEFRAG 0xA. Frames: 80211_mpdu = raw management frame; else Ethernet II (decrypted); amsdu =
subframes {DA, SA, be16 len, body (LLC/SNAP), pad 4}.
Give space back: WRITE_SRAM 8 B {u32 1, u32 tail (with BIT31 phase) | flags} to 0x00a10038 (or piggyback on a CBW:
bytes 432-435 = 0x11223344, 436-439 = value).
Buffer handshake: TX_STOP (ext 1): stop TX, when all pages are back WRITE_SRAM {3, 1} to 0x00a10034; NARROW (BIT20):
drain, head/tail back to start if >= 0x60029130, end 0x60029130, 77 TX pages, confirm with BIT27; when BIT20 clears
confirm with BIT28. EXPAND (BIT21): end 0x600684b0, 21 pages, BIT25; then BIT26. TX_START (ext 2): resume.

## Boot messages (dest = the id's task, src 100)
| # | request | params | reply |
|---|---|---|---|
| 1 | MM_RESET_REQ 0x0000 | 0 | MM_RESET_CFM 0x0001 |
| 2 | MM_VERSION_REQ 0x0004 | 0 | MM_VERSION_CFM 0x0005: version_lmac u32 @0, machw_1 @4, machw_2 @8, phy_1 @12 (19:16 RF mode, 11:8 streams, 25:24 bw), phy_2 @16, features @20 (UMAC bit8, AMSDU bit5, BCN bit0, BFMER bit11), max_sta u16 @24 (10), max_vif u8 @26 (4) |
| 3 | ME_CONFIG_REQ 0x1400 | 116 | ME_CONFIG_CFM 0x1401 |
| 4 | MM_OTHER_REQ 0x0069 sub 35 MM_EXT_CAPAB | 16: {35, 04 00 40 00 00 00 00 C0 01 00} | none |
| 5 | ME_CHAN_CONFIG_REQ 0x1402 | 256 | 0x1403 |
| 6 | MM_OTHER_REQ sub 11 MM_SUB_GET_EFUSE | 8: {11, u32 addr} | PRIV_EFUSE_GET_RESULT 0x400A {u32 addr, u32 value} |
| 7 | MM_OTHER_REQ sub 29 SET_TEMP_START | 4 | none |
| 8 | MM_START_REQ 0x0002 | 80: phy_cfg[16] u32 = 0, uapsd_timeout u32 @64 = 300, lp_clk_accuracy u16 @68 = 20, tx_timeout[4] @70 = 0, rx_hostbuf_size @78 = 0 | MM_START_CFM 0x0003 |
| 9 | MM_ADD_IF_REQ 0x0006 | 12: type @0 = 0 (STA), mac @2, p2p @8 = 0 | MM_ADD_IF_CFM 0x0007 {status, inst_nbr} |
| 10 | MM_OTHER_REQ sub 10 SET_CALI | 424 (Cali_Param from aml_wifi_rf.txt) | none |

ME_CONFIG_REQ (2 streams, 80 MHz example): ht_capa_info u16 @0 0x09E3; a_mpdu_param @2 0x1F; mcs_rate[16] @3
FF FF 00 00 01 00 00 00 00 00 2C 01 01 00 00 00; ht_ext @20 0; txbf u32 @24 0; asel @28 0; vht_capa_info u32 @32
0x038071B2; rx_mcs_map u16 @36 0xFFFA; rx_highest @38 780; tx_mcs_map @40 0xFFFA; tx_highest @42 780; he mac_cap[6] @44
00 00 02 00 00 00; he phy_cap[11] @50 06 E0 2B 78 0D C0 CF 04 82 32 00; he mcs @62 FFFA FFFA FFFF FFFF FFFF FFFF;
ppe_thres[25] @74 39 1C C7 71 1C 07 00..; tx_lft u16 @100 1000; phy_bw_max @102 2; ht_supp @103 1; vht_supp @104 1;
he_supp @105 1; he_ul_on @106 0; ps_on @107 1; ant_div_on @108 0; dpsm @109 1; amsdu_tx int @112 0.
MAC: efuse words through the firmware: valid = (efuse[7] >> 17) & 1; lo, hi = valid ? efuse[0x11], efuse[0x12] :
efuse[1], efuse[2]; MAC = hi[15:8], hi[7:0], lo[31:24], lo[23:16], lo[15:8], lo[7:0]; byte0 &= 0xFC.
