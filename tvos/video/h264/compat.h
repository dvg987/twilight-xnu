/* Userspace compatibility for the Amlogic DPB. Calls are serialized by the decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
struct vdec_frames_s { u32 frame_size; u64 hw_decode_time; };
struct vdec_s { void *priv; struct vdec_frames_s *mvfrm; };
#define GFP_ATOMIC 0
#define kzalloc(size,flags) calloc(1,size)
#define kfree(p) free(p)
#define container_of(p,type,field) ((type *)((char *)(p)-offsetof(type,field)))
#define spin_lock_irqsave(p,flags) do { (void)(p); (flags)=0; } while(0)
#define spin_unlock_irqrestore(p,flags) do { (void)(p); (void)(flags); } while(0)
#define pr_cont(...) fprintf(stderr,__VA_ARGS__)
#define pr_debug(...) do {} while(0)
#define qsort amlh264_qsort

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
