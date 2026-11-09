#!/usr/bin/env python3
"""Exercise the production DPB adapter with captured I/P slice headers and sanitizers."""
import base64
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
# Firmware parameter blocks, not firmware code: IDR POC 0, then P POC 6 referencing it.
VECTORS = [
    'eJxjYKAcsIBJRiwy95hgLC4ssth0EA/+10MwuokQ13AwMEFZMMCOxGZiYIayWBmYwSpBOtnAIj/+g9jsDIwMggwscHXI4JMApk+YGHghrpJHlWFmYIG6o+ovhM+KJMvPQC0A9WsKFwPIcbBg1wGH0/96BkZlaGgzosWFr6MgI0gXSAJkBhvYvSxIrmQG62FE0csBJTFDeRRQA6RCYxCefUbBKKAJ8HVkUGASEWBg4WQczckMIw8AAE/YEIU=',
    'eJztkz0OwjAMhZ/z05ZKiILYQYKZO/QA3IJTsPTOTCyoyI6pmpIBiTKRT4rlxrHznKTA9zixlIicBq9ORFMZn9N3YUwrBjUVjHovytEqD6u7Eww8nHwVMnPv2S9B2MDBJna+NVP9XGUZVO3itRZOdVwfbE3U9Qpzob1earA4o7NHOae+Ax2GfuO7OLdr4iwOcI1C9Dr4UQecQ1Fupfb9lDNz0Mo7Ce8xk/kdFtibbQO3oPwn4/94AjMWDyE=',
]
HARNESS = r'''
#include "compat.h"
#include "h264_dpb.h"
#include "dpb.h"
#include <sys/hakios.vdec.h>
#include <assert.h>
static void rejected(const uint16_t *rpm) {
    struct twl_h264_dpb *d=twl_h264_dpb_create(); assert(d);
    struct twl_h264_config c;
    assert(twl_h264_dpb_prepare(d,rpm,&c)<0);
    twl_h264_dpb_destroy(d);
}
int main(int argc,char **argv) {
    assert(argc==3);
    uint16_t negative[2]={0xffff,0xffff}, minimum[2]={0,0x8000};
    assert(val(negative)==-1 && val(minimum)==INT32_MIN);
    uint16_t vectors[2][1024],bad[1024];
    for(int i=0;i<2;i++) {
        FILE *f=fopen(argv[i+1],"rb"); assert(f);
        assert(fread(vectors[i],1,2048,f)==2048); fclose(f);
    }
    for(int run=0;run<8;run++) {
        struct twl_h264_dpb *d=twl_h264_dpb_create(); assert(d);
        for(int gop=0;gop<32;gop++) {
            struct twl_h264_config i,p;
            assert(twl_h264_dpb_prepare(d,vectors[0],&i)==0);
            assert(i.frame_poc==0 && !i.reference_count[0] && !i.reference_count[1]);
            assert(twl_h264_dpb_complete(d)==0);
            assert(twl_h264_dpb_prepare(d,vectors[1],&p)==0);
            assert(p.frame_poc==6 && p.reference_count[0]==1 && !p.reference_count[1]);
            assert(p.references[0][0]==i.current_slot);
            assert(p.current_slot!=i.current_slot);
            assert(twl_h264_dpb_complete(d)==0);
        }
        twl_h264_dpb_destroy(d);
    }
    rejected(vectors[1]); /* A fresh context cannot decode P without references. */
    memcpy(bad,vectors[0],sizeof(bad)); bad[LOG2_MAX_FRAME_NUM]=0; rejected(bad);
    memcpy(bad,vectors[0],sizeof(bad)); bad[NUM_REF_FRAMES_IN_PIC_ORDER_CNT_CYCLE]=129; rejected(bad);
    memcpy(bad,vectors[0],sizeof(bad)); bad[FRAME_MBS_ONLY_FLAG]=0; rejected(bad);
    memcpy(bad,vectors[0],sizeof(bad)); bad[FIRST_MB_IN_SLICE]=1; rejected(bad);
    puts("H.264 DPB: 512 I/P pictures, context reuse and malformed-header checks: PASS");
    return 0;
}
'''


def main():
    scratch = ROOT / 'work/vdec-tmp'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dpb-', dir=scratch) as directory:
        directory = Path(directory)
        include = directory / 'sys'
        include.mkdir()
        shutil.copyfile(ROOT / 'hakios/xnu/files/bsd/sys/hakios.vdec.h', include / 'hakios.vdec.h')
        (include / 'ioccom.h').write_text('#include <sys/ioctl.h>\n')
        (directory / 'test.c').write_text(HARNESS)
        for i, vector in enumerate(VECTORS):
            data = zlib.decompress(base64.b64decode(vector))
            assert len(data) == 2048
            (directory / f'{i}.rpm').write_bytes(data)
        source = ROOT / 'tvos/video/h264'
        env = dict(os.environ, TMPDIR=str(directory), ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        subprocess.run(['clang', '-O1', '-g', '-Wno-pointer-sign', '-fsanitize=address,undefined',
                        '-I'+str(directory), '-I'+str(source), str(directory/'test.c'),
                        str(source/'dpb.c'), str(source/'h264_dpb.c'), '-o', str(directory/'test')],
                       env=env, check=True)
        subprocess.run([str(directory/'test'), str(directory/'0.rpm'), str(directory/'1.rpm')], env=env, check=True)


if __name__ == '__main__':
    main()
