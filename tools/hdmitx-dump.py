#!/usr/bin/env python3
"""The HDMI transmitter's registers from the box (sysctl kern.hakios_hdmitx_dump, written by hakios.hdmitx.c).

  tools/hdmitx-dump.py get FILE [boot]     fetch a dump over SSH (tools/twl-ssh.sh); "boot": the one taken at boot
  tools/hdmitx-dump.py show FILE           the registers that matter for the output, by name
  tools/hdmitx-dump.py diff A B            every register that differs (cor/top/mmio/vcbus, infoframes, SCDC)
"""
import struct
import subprocess
import sys

NAMES = {
    ('clk', 0x04c): 'SYS_CLK_EN0_REG2', ('clk', 0x0c0): 'VID_CLK0_CTRL', ('clk', 0x0c4): 'VID_CLK0_CTRL2',
    ('clk', 0x0c8): 'VID_CLK0_DIV', ('clk', 0x0dc): 'ENC_HDMI_CLK_CTRL', ('clk', 0x0e0): 'HDMI_CLK_CTRL',
    ('clk', 0x0e4): 'VID_PLL_CLK_DIV', ('clk', 0x11c): 'HTX_CLK_CTRL0', ('clk', 0x120): 'HTX_CLK_CTRL1',
    ('ana', 0x1c0): 'HDMIPLL_CTRL0', ('ana', 0x1c4): 'HDMIPLL_CTRL1', ('ana', 0x1c8): 'HDMIPLL_CTRL2',
    ('ana', 0x1cc): 'HDMIPLL_CTRL3', ('ana', 0x200): 'HDMIPHY_CTRL0', ('ana', 0x204): 'HDMIPHY_CTRL1',
    ('ana', 0x20c): 'HDMIPHY_CTRL3', ('ana', 0x214): 'HDMIPHY_CTRL5', ('ana', 0x21c): 'HDMIPHY_CTRL6',
    ('pwr', 0x06c): 'MEM_PD11', ('pad', 0x02c): 'PIN_MUX_B', ('pad', 0x140): 'GPIOH_I',
    ('vcbus', 0x1a1b): 'OSD1_BLK0_CFG_W0', ('vcbus', 0x1a14): 'OSD1_BLK1_CFG_W4', ('vcbus', 0x1a15): 'OSD1_BLK2_CFG_W4',
    ('vcbus', 0x1b57): 'ENCI_VIDEO_EN', ('vcbus', 0x1b80): 'ENCP_VIDEO_EN', ('vcbus', 0x1b81): 'ENCP_VIDEO_SYNC_MODE',
    ('vcbus', 0x1b8d): 'ENCP_VIDEO_MODE', ('vcbus', 0x1b8e): 'ENCP_VIDEO_MODE_ADV', ('vcbus', 0x1b97): 'ENCP_VIDEO_MAX_PXCNT',
    ('vcbus', 0x1bae): 'ENCP_VIDEO_MAX_LNCNT', ('vcbus', 0x1ba3): 'ENCP_VIDEO_HAVON_END', ('vcbus', 0x1ba4): 'ENCP_VIDEO_HAVON_BEGIN',
    ('vcbus', 0x1ba6): 'ENCP_VIDEO_VAVON_BLINE', ('vcbus', 0x1baf): 'ENCP_VIDEO_VAVON_ELINE', ('vcbus', 0x1ba7): 'ENCP_VIDEO_HSO_BEGIN',
    ('vcbus', 0x1ba8): 'ENCP_VIDEO_HSO_END', ('vcbus', 0x1bab): 'ENCP_VIDEO_VSO_BLINE', ('vcbus', 0x1bac): 'ENCP_VIDEO_VSO_ELINE',
    ('vcbus', 0x1c30): 'ENCP_DVI_HSO_BEGIN', ('vcbus', 0x1c31): 'ENCP_DVI_HSO_END', ('vcbus', 0x1c32): 'ENCP_DVI_VSO_BLINE_EVN',
    ('vcbus', 0x1c34): 'ENCP_DVI_VSO_ELINE_EVN', ('vcbus', 0x1c36): 'ENCP_DVI_VSO_BEGIN_EVN', ('vcbus', 0x1c38): 'ENCP_DVI_VSO_END_EVN',
    ('vcbus', 0x1c3a): 'ENCP_DE_H_BEGIN', ('vcbus', 0x1c3b): 'ENCP_DE_H_END', ('vcbus', 0x1c3c): 'ENCP_DE_V_BEGIN',
    ('vcbus', 0x1c3d): 'ENCP_DE_V_END', ('vcbus', 0x1cef): 'VPU_VENC_CTRL', ('vcbus', 0x1dc9): 'VPP_OSD_SCI_WH_M1',
    ('vcbus', 0x1dca): 'VPP_OSD_SCO_H', ('vcbus', 0x1dcb): 'VPP_OSD_SCO_V', ('vcbus', 0x271a): 'VPU_VIU_VENC_MUX_CTRL',
    ('vcbus', 0x271b): 'VPU_HDMI_SETTING', ('vcbus', 0x2743): 'VPU_HDMI_FMT_CTRL', ('vcbus', 0x27fc): 'VPU_HDMI_DITH_CNTL',
    ('vcbus', 0x38a0): 'OSD1_HDR2_CTRL',
    ('top', 0x000): 'TOP_SW_RESET', ('top', 0x004): 'TOP_CLK_CNTL', ('top', 0x008): 'TOP_HPD_FILTER', ('top', 0x00c): 'TOP_INTR_MASKN',
    ('top', 0x018): 'TOP_BIST_CNTL', ('top', 0x028): 'TOP_TMDS_CLK_PTTN_01', ('top', 0x02c): 'TOP_TMDS_CLK_PTTN_23',
    ('top', 0x030): 'TOP_TMDS_CLK_PTTN_CNTL', ('top', 0x074): 'TOP_INFILTER', ('top', 0x08c): 'TOP_HV_ACTIVE',
    ('cor', 0x018): 'AON_CYP_CTL', ('cor', 0x10b): 'SOC_FUNC_SEL', ('cor', 0x10d): 'CLKPWD', ('cor', 0x110): 'PWD_SRST',
    ('cor', 0x12f): 'CLK_DIV_CNTRL', ('cor', 0x235): 'CLKRATIO', ('cor', 0x236): 'P2T_CTRL', ('cor', 0x2ca): 'TEST_TXCTRL',
    ('cor', 0x61a): 'TPI_SC', ('cor', 0x6e1): 'GCP_CNTL', ('cor', 0x6e3): 'GCP_AUTO_GEN', ('cor', 0x900): 'SCRCTL',
    ('cor', 0xb18): 'VP_INPUT_MASK', ('cor', 0xb44): 'VP_OUTPUT_MAPPING', ('cor', 0xb45): 'VP_OUTPUT_MAPPING+1',
    ('cor', 0xb46): 'VP_OUTPUT_MASK',
}
SLOTS = {0: 'AVI', 1: 'EMP', 2: 'audio', 3: 'SPD', 5: 'vendor', 6: 'DRM', 7: 'GCP', 8: 'HF vendor', 10: 'SBTM'}


def parse(blob):
    """{section key: {address: value}}, frames {slot: (bytes, enable)}, scdc {offset: value}"""
    assert blob[:4] == b'HDTX', 'not a dump'
    at, regs, frames, scdc = 8, {}, {}, {}
    while at + 24 <= len(blob):
        name = blob[at:at + 8].rstrip(b'\0').decode()
        base, count, width = struct.unpack_from('<III', blob, at + 8)
        at += 24
        data = blob[at:at + count * width]
        at += count * width
        if name == 'frames':
            for s in range(count // 32):
                frames[s] = (data[s * 32:s * 32 + 31], data[s * 32 + 31])
        elif name == 'scdc':
            scdc = {k: data[k] for k in range(count)}
        elif width == 4:
            vals = struct.unpack('<%dI' % count, data)
            key = name if name != 'vcbus' else 'vcbus'
            sect = regs.setdefault(key, {})
            for k, v in enumerate(vals):
                sect[(base + k) if name == 'vcbus' else k * 4] = v
        else:
            sect = regs.setdefault(name, {})
            for k, v in enumerate(data):
                sect[k] = v
    return regs, frames, scdc


def label(sect, addr):
    n = NAMES.get((sect, addr))
    return '%-5s %#06x%s' % (sect, addr, (' ' + n) if n else '')


def show(path):
    regs, frames, scdc = parse(open(path, 'rb').read())
    for (sect, addr), _ in sorted(NAMES.items(), key=lambda x: (x[0][0], x[0][1])):
        if sect in regs and addr in regs[sect]:
            v = regs[sect][addr]
            print('%-40s %s' % (label(sect, addr), ('%#04x' % v) if sect == 'cor' else ('%#010x' % v)))
    for s, (f, en) in frames.items():
        if en or any(f):
            print('frame %2d %-9s en %#04x: %s' % (s, SLOTS.get(s, ''), en, f.hex(' ')))
    if scdc:
        print('SCDC: ' + ' '.join('%02x:%02x' % (k, v) for k, v in scdc.items() if v not in (0, 0xee)))


def diff(a, b):
    ra, fa, sa = parse(open(a, 'rb').read())
    rb, fb, sb = parse(open(b, 'rb').read())
    for sect in ra:
        for addr in sorted(ra[sect]):
            va, vb = ra[sect][addr], rb.get(sect, {}).get(addr)
            if va != vb:
                fmt = '%#04x' if sect == 'cor' else '%#010x'
                print('%-40s %s -> %s' % (label(sect, addr), fmt % va, fmt % vb if vb is not None else '-'))
    for s in fa:
        if fa[s] != fb.get(s):
            print('frame %2d %-9s\n   %s en %#04x\n   %s en %#04x' % (s, SLOTS.get(s, ''), fa[s][0].hex(' '), fa[s][1],
                                                                fb[s][0].hex(' ') if s in fb else '-', fb[s][1] if s in fb else 0))
    for k in sorted(set(sa) | set(sb)):
        if sa.get(k) != sb.get(k):
            print('scdc  %#04x %s -> %s' % (k, sa.get(k), sb.get(k)))


def get(path, boot):
    name = 'kern.hakios_hdmitx_bootdump' if boot else 'kern.hakios_hdmitx_dump'
    out = subprocess.run(['tools/twl-ssh.sh', '/var/jb/usr/sbin/sysctl -b ' + name], capture_output=True, check=True).stdout
    open(path, 'wb').write(out)
    print('%s: %d bytes' % (path, len(out)))


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'get':
        get(sys.argv[2], len(sys.argv) > 3 and sys.argv[3] == 'boot')
    elif len(sys.argv) == 3 and sys.argv[1] == 'show':
        show(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'diff':
        diff(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(1)
