# Amlogic W2 FullMAC over USB: station operations

Condensed from Amlogic's driver (LineageOS android_kernel_amlogic_amlogic-drivers_w2, aml_drv/; layouts compiled from
its headers, LP64 little-endian, natural alignment unless noted). Citations are file:line in that tree.

## Messages
- `struct lmac_msg` (8 B): id u16 @0, dest_id u16 @2, src_id u16 @4 (= DRV_TASK_ID 100), param_len u16 @6 (rounded up
  to 4, padding zero), param[] @8. id = task << 10 | index. A2E message fits 508 bytes (params <= 500).
- Tasks: MM 0, DBG 1, SCAN 2, TDLS 3, SCANU 4, ME 5, SM 6, APM 7, BAM 8, MESH 9, RXU 10, RM 11, TWT 12, FTM 13, API 14;
  PRIV 16 (FW->host only, 0x4000+). Aml sub-commands: MM_OTHER_REQ 0x0069 (dest MM), param { u32 sub_index; u8 p[] }.
- CFM status: CO_OK 0, FAIL 1, EMPTY 2, FULL 3, BAD_PARAM 4, NOT_FOUND 5, NO_MORE_ELT 6, NO_ELT_IN_USE 7, BUSY 8,
  OP_IN_PROGRESS 9. Timeout used by Linux: 6 s.

| Host -> FW | id | dest | answer |
|---|---|---|---|
| SCANU_START_REQ | 0x1000 | 4 | none; end = SCANU_START_CFM 0x1001 |
| MM_OTHER_REQ sub 5 (scan cancel) | 0x0069 | 0 | PRIV_SCANU_CANCEL_CFM 0x4001 |
| SM_CONNECT_REQ | 0x1800 | 6 | SM_CONNECT_CFM 0x1801 {status u8}, then SM_CONNECT_IND 0x1802 |
| SM_DISCONNECT_REQ | 0x1803 | 6 | SM_DISCONNECT_CFM 0x1804, then SM_DISCONNECT_IND 0x1805 |
| SM_EXTERNAL_AUTH_REQUIRED_RSP | 0x1807 | 6 | none |
| MM_KEY_ADD_REQ | 0x0024 | 0 | MM_KEY_ADD_CFM 0x0025 {status u8, hw_key_idx u8} |
| MM_KEY_DEL_REQ | 0x0026 | 0 | MM_KEY_DEL_CFM 0x0027 |
| ME_SET_CONTROL_PORT_REQ | 0x1404 | 5 | ME_SET_CONTROL_PORT_CFM 0x1405 |
| ME_CHAN_CONFIG_REQ | 0x1402 | 5 | ME_CHAN_CONFIG_CFM 0x1403 |

FW -> host to handle as a station: SCANU_RESULT_IND 0x1004, SM_EXTERNAL_AUTH_REQUIRED_IND 0x1806 (must answer, status 1
if no SAE), MM_CHANNEL_PRE_SWITCH_IND 0x0045 {chan_index} (stop TX), MM_CHANNEL_SWITCH_IND 0x0044 {chan_index, roc,
vif_index, roc_tdls} (roc 0: TX may run on that context), MM_CSA_TRAFFIC_IND 0x0059 {vif, enable},
MM_CSA_FINISH_IND 0x0058, PRIV_COEX_STOP_RESTORE_TXQ_IND 0x4013 {u8 1 pause / 0 resume},
ME_TX_CREDITS_UPDATE_IND 0x140B {sta_idx, tid, s8 credits} (initial credits 4), MM_BA_ADD_REQ 0x0028 /
MM_BA_DEL_REQ 0x002A (from FW: RX reorder sessions), ME_TKIP_MIC_FAILURE_IND 0x1406. Informational: DBG_ERROR_IND
0x0408, MM_RSSI_STATUS_IND 0x0057, MM_PKTLOSS_IND 0x0060, PRIV_CONNECT_INFO 0x4006, MM_PS_CHANGE_IND 0x0049.
USB: IRQ status bit 15 = a channel switch indication in SRAM at 0x00A17FC0 {u32 msg_id; u8 chan_index; bool roc;
u8 vif_index; bool roc_tdls} (ipc_host.c:163-192).

## Shared structs
- mac_chan_def (6 B): freq u16 @0, band u8 @2 (0 2.4 GHz, 1 5 GHz), flags u8 @3 (NO_IR 1, DISABLED 2, RADAR 4),
  tx_power s8 @4.
- mac_chan_op (10 B): band @0, type @1 (BW20 0, 40 1, 80 2, 160 3), prim20 u16 @2, center1 u16 @4, center2 u16 @6,
  tx_power s8 @8, flags @9.
- mac_ssid (33 B): length @0, bytes @1. mac_addr: 6 bytes, on-air order.
- mac_sec_key (36 B, align 4): length u8 @0, key bytes @4.

## Scan: SCANU_START_REQ (340 B)
chan[42] mac_chan_def @0; ssid[2] mac_ssid @252; bssid @318 (ff:..); add_ies u32 @324 (0 on USB); add_ie_len u16 @328
(<= 200); vif_idx @330; chan_cnt @331; ssid_cnt @332; no_cck @333; duration u32 @336 (us per channel, 0 = FW default).
Channel flags: NO_IR when ssid_cnt == 0 (so zero SSIDs = passive everywhere; active wildcard = ssid_cnt 1, length 0),
plus the channel's NO_IR/RADAR; tx_power 20. Probe-request extra IEs: written to SRAM 0x6000F42C (SCANU_ADD_IE + 16)
with the write-SRAM command before the request.
Results: beacons/probe responses arrive on the RX *data* path as 802.11 MPDUs (is_80211_mpdu = 1), with freq
(prim20) / band / rssi1 from the RX header; SCANU_RESULT_IND 0x1004 (16 B header: length u16, framectrl u16,
center_freq u16, band u8, sta_idx u8, inst_nbr u8, rssi s8, rx_leg_inf 3 B packed, payload @16 = the frame) may also
carry one inline. End: SCANU_START_CFM 0x1001 {vif_idx, status, result_cnt}.

## Connect: SM_CONNECT_REQ (64 B + IEs inline)
ssid @0; bssid @34 (or ff..); chan mac_chan_def @40 (freq 0xFFFF if unknown); flags u32 @48; ctrl_port_ethertype @52
(network order: bytes 88 8E); listen_interval u16 @54 (0); dont_wait_bcmc @56 (0); auth_type @57 (OPEN 0, SHARED 1,
FT 2, SAE 3); uapsd_queues @58 (0); vif_idx @59; ie_len u16 @60; ie_buf @64 (<= 436 B).
flags: CONTROL_PORT_HOST 0x01, CONTROL_PORT_NO_ENC 0x02, DISABLE_HT 0x04 (WEP/TKIP pairwise), WPA_WPA2_IN_USE 0x08
(set whenever the group cipher is not WEP, open networks too), MFP_IN_USE 0x10, REASSOCIATION 0x20, FT_OVER_DS 0x40,
WPA3_SAE_IN_USE 0x80. WPA2-PSK CCMP: flags 0x09, IEs = RSN 30 14 01 00 00 0F AC 04 01 00 00 0F AC 04 01 00 00 0F AC 02
<caps 2 B>.
SM_CONNECT_IND (48 B + IEs): status_code u16 @0 (0 ok); bssid @2; roamed @8; vif_idx @9; ap_idx @10 (the AP's station
index: TX staid, key sta_idx, control port); ch_idx @11; qos @12; acm @13; assoc_req_ie_len u16 @14;
assoc_rsp_ie_len u16 @16; aid u16 @18; chan mac_chan_op @20; ac_param u32[4] @32; IEs @48. TX starts stopped unless
ch_idx is the current channel context (MM_CHANNEL_SWITCH_IND).
The host runs the 4-way handshake: EAPOL (0x888E) as ordinary 802.3 data both ways (TID 7 when the AP does QoS).
Then MM_KEY_ADD PTK, MM_KEY_ADD GTK, ME_SET_CONTROL_PORT_REQ(ap_idx, 1).

## Keys and port
MM_KEY_ADD_REQ (44 B): key_idx @0 (PTK 0, GTK 1-3); sta_idx @1 (PTK ap_idx, GTK 0xFF); key mac_sec_key @4 (length @4,
bytes @8); cipher_suite @40 (WEP40 0, TKIP 1, CCMP 2, WEP104 3, SMS4 4, BIP 5, GCMP128 6, GCMP256 7, CCMP256 8);
inst_nbr @41 (vif); spp @42 (0); pairwise @43. No RSC sent. MM_KEY_DEL_REQ {hw_key_idx u8}.
ME_SET_CONTROL_PORT_REQ (2 B): sta_idx @0, control_port_open @1.
SM_DISCONNECT_REQ (4 B): reason_code u16 @0, vif_idx @2. SM_DISCONNECT_IND (4 B): reason u16 @0 (bit 15 = asked by
the host; 40 = link loss), vif_idx @2, reassoc @3 (1: don't report).

## Data frames
TX (USB): struct txdesc_host (72 B) then the payload without its 14-byte Ethernet header. hostdesc: hostid u32 @0
(nonzero cookie, echoed in confirms); packet_addr[6] @4 ([0] = 1 on USB); packet_len[6] u16? @28 ([0] = length - 14);
packet_cnt @40 (1); eth_dest_addr @42; eth_src_addr @48; ethertype @54 (network order); flags u16 @56 (0; SP_FRAME
0x400 for EAPOL/ARP/DHCP; AMSDU 0x40); sn_for_retry @58; tid @60 (QoS AP: 802.1d priority, EAPOL 7; else 0xFF);
vif_idx @61; staid @62 (ap_idx, broadcast too); ctrl.hwq @64 (AC by TID {BE,BK,BK,BE,VI,VI,VO,VO}; BK 0 BE 1 VI 2
VO 3; tid 0xFF -> BE); ready u32 @68 = 0xFFFFFFFF. Send only with credits and TX not stopped.
RX: each FW RX descriptor is 128 B with struct hw_rxhdr (52 B) at +48; payload at desc + 128 (+36 if frmlen); frame at
payload + payl_offset (payload begins with aml_rhd_ext: sn:12 fn:4, flags, optional u64 PN).
hw_rxhdr: hwvect.len bits 0-15 @0; payl_offset u8 @4; status u8 @5 (FORWARD 1, ALLOC 2, DELETE 4, LEN_UPDATE 8,
SPURIOUS 0x40; HOST_REO 0x03; DEFRAG 0x0A); rx_vect1 @12 (rssi1 = byte 17, s8 dBm); new_read u32 @28; mpdu_status @36;
phy_band @40, channel_type @41, prim20_freq u16 @42, center1 @44, center2 @46; flags @48: bit0 is_amsdu,
bit1 is_80211_mpdu, bit2 4addr, bit3 new_peer, bits 4-6 TID, 8-15 vif_idx, 16-23 sta_idx, 24-31 dst_idx.
is_80211_mpdu 1: raw management frame (beacons...). Else and not A-MSDU: an 802.3 frame, decrypted. A-MSDU: subframes
to split. HOST_REO: reorder per (sta, tid) by sn (optional to start with).

## Channels: ME_CHAN_CONFIG_REQ (254 B)
chan2G4[14] mac_chan_def @0, chan5G[28] @84, chan2G4_cnt @252, chan5G_cnt @253. 2.4 GHz: 2412..2472 step 5, 2484.
5 GHz: 5180..5320 step 20 (36-64), 5500..5720 step 20 (100-144), 5745..5885 step 20 (149-177). tx_power 20; RADAR (4)
on 52-64 and 100-144; 5845/5865/5885 DISABLED (2) under the world domain.

## Minimal station sequence
ME_CHAN_CONFIG -> SCANU_START (collect MPDUs until SCANU_START_CFM) -> SM_CONNECT_REQ / CFM / IND -> EAPOL 4-way as
data (staid ap_idx, TID 7) -> MM_KEY_ADD PTK, GTK -> ME_SET_CONTROL_PORT(ap_idx, 1) -> data. Leave: SM_DISCONNECT_REQ
(reason 3).
