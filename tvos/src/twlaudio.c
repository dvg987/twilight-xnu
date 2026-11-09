/*
 * TwlAudio.driver: the HDMI output of tvOS on the Xiaomi Mi Box S Gen 3 ("twilight"), as an AudioServerPlugIn.
 *
 * Apple's own path is IOAudio2 (AppleAE2Audio, an I2S block) into a DisplayPort transmitter and a DP-to-HDMI bridge, none of which
 * this box has; mediaserverd also reads the physical devices from the HAL, so what stands in for them is a plug-in in
 * /System/Library/Audio/Plug-Ins/HAL (init binds our /hal over it), as the phone port does (hakios/userland/src/audio_plugin.c).
 * This one presents a single output device whose UID is "HDMI", the name the virtual audio device (VirtualAudio.plugin,
 * Device_HDMI) builds the HDMI output port from. Its samples go to the kernel (/dev/twl-audio, hakios.hdmiaudio.c), which feeds the
 * HDMI transmitter. The stream offers 16-bit LPCM at 48 kHz in the channel counts the TV or receiver lists in its EDID (2, then 6 and
 * 8), and the two non-mixable formats tvOS sets on a device (read from MediaExperience's CMVAEndptMgr): 'cac3' (AC-3 in IEC 60958, 48 kHz,
 * 2 x 16 bit, 1536 frames = 6144 bytes a packet; Settings > Audio Format > Change Format > Dolby Digital 5.1) and 'mtb+' (Dolby MAT, the
 * Atmos carrier: 32 channels x 16 bit at 48 kHz, 960 frames = 61440 bytes a packet, which is the 8-channel 192 kHz HBR stream byte for byte).
 * Built without -DTWL_NO_PASSTHROUGH. The device clock is the
 * kernel's: the frames the hardware has taken.
 * Every format the audio server asks for is logged (stderr), because what tvOS asks for first is what settles the rest.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hakios.hdmiaudio.h"

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};


/* ---- the device ---------------------------------------------------------------------------------- */

enum { OBJECT_PLUGIN = 1, OBJECT_DEVICE, OBJECT_STREAM, OBJECT_VOLUME, OBJECT_LAST = OBJECT_VOLUME };

#ifndef TWL_DEVICE_UID
#define TWL_DEVICE_UID "HDMI"           /* what the virtual audio device knows the HDMI output by (VirtualAudio: "HDMI", Device_HDMI) */
#endif
#define MANUFACTURER            CFSTR("twilight")
#define TRANSPORT_HDMI          'hdmi'
#define TIMESTAMP_PERIOD        16384
#define kFlagNonMixable         (1u << 6)       /* kAudioFormatFlagIsNonMixable (macOS CoreAudioBaseTypes.h) */
#define kFormat60958AC3         'cac3'          /* kAudioFormat60958AC3: AC-3 in an IEC 60958 frame, 2 channels */
#define TWL_RATE                48000

/* The physical formats the stream offers: linear PCM in the channel counts the sink takes, and (TWL_PASSTHROUGH) AC-3 */
struct pformat {
	UInt32 id, flags, channels, kind;
	UInt32 bytes_per_packet, frames_per_packet, bytes_per_frame;
	UInt32 hw_channels, hw_rate, rate_div;          /* what the kernel is told, and how many of its frames make one of ours */
};
#define PCM_FORMAT(ch) { kAudioFormatLinearPCM, pcm_flags, ch, TWL_AUDIO_PCM, 2 * (ch), 1, 2 * (ch), ch, TWL_RATE, 1 }
#define kFormatMAT              'mtb+'          /* tvOS sets 'mtb+' or 'mtc+' (it prefers 'mtc+' when the device lists it) */
static struct pformat formats[8];
static SInt32 hog_pid = -1;
static unsigned format_count, current;

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;
static Float64 ticks_per_frame;
static unsigned running;
static UInt64 anchor, periods, written, refused;
static Boolean inactive;
static Float32 volume = 1.0f;
static Boolean have_position;
static int pcm_flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static AudioStreamBasicDescription
format_desc(const struct pformat *f)
{
	return (AudioStreamBasicDescription) {
		       .mSampleRate = TWL_RATE, .mFormatID = f->id, .mFormatFlags = f->flags,
		       .mBytesPerPacket = f->bytes_per_packet, .mFramesPerPacket = f->frames_per_packet, .mBytesPerFrame = f->bytes_per_frame,
		       .mChannelsPerFrame = f->channels, .mBitsPerChannel = 16,
	};
}

/* What the sink takes (the kernel reads the EDID): 2 channels always, 6 and 8 where a descriptor lists LPCM with that many */
static void
make_formats(void)
{
	struct twl_audio_caps caps;
	unsigned max_pcm = 2;
	Boolean ac3 = false, mat = false;

	memset(&caps, 0, sizeof(caps));
	if (sink >= 0 && ioctl(sink, TWL_AUDIO_GET_CAPS, &caps) == 0) {
		for (UInt32 i = 0; i < caps.count; i++) {
			if (caps.sad[i].format == 1 && (caps.sad[i].rates & 4) && caps.sad[i].channels > max_pcm) {
				max_pcm = caps.sad[i].channels;
			}
			if (caps.sad[i].format == 2 && (caps.sad[i].rates & 4)) {
				ac3 = true;
			}
			if (caps.sad[i].format == 12 && caps.sad[i].channels >= 8) {
				mat = true;
			}
		}
		say("twilight: audio: the sink takes up to %u channels of LPCM%s%s (%u descriptors, speakers %#x)\n", max_pcm,
		    ac3 ? ", AC-3" : "", mat ? ", MAT" : "", caps.count, caps.speakers);
	} else {
		say("twilight: audio: no capabilities from the kernel: stereo only\n");
	}
	format_count = 0;
	{
		struct pformat f2 = PCM_FORMAT(2), f6 = PCM_FORMAT(6), f8 = PCM_FORMAT(8);

		formats[format_count++] = f2;
		if (max_pcm >= 6) {
			formats[format_count++] = f6;
		}
		if (max_pcm >= 8) {
			formats[format_count++] = f8;
		}
	}
#ifndef TWL_NO_PASSTHROUGH
	if (ac3) {
		/* Dolby Digital 5.1: tvOS's own encoder, IEC 60958 framing: 'cac3', non-mixable, 48 kHz only */
		formats[format_count++] = (struct pformat) { kFormat60958AC3, pcm_flags | kFlagNonMixable, 2, TWL_AUDIO_IEC61937,
			                                     6144, 1536, 4, 2, TWL_RATE, 1 };
	}
	if (mat) {
		/* Dolby Atmos: MAT as 32 channels of 16 bit at 48 kHz, which is the HBR stream (8 channels at 192 kHz) in the same bytes */
		formats[format_count++] = (struct pformat) { kFormatMAT, pcm_flags | kFlagNonMixable, 32, TWL_AUDIO_HBR,
			                                     61440, 960, 64, 8, 192000, 4 };
	}
#endif
	current = 0;
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;
	return S_OK;
}

static ULONG add_ref(__unused void *driver) { return 1; }
static ULONG release(__unused void *driver) { return 1; }

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / TWL_RATE;
	sink = open(TWL_AUDIO_DEVICE, O_WRONLY);
	say("twilight: audio: the HDMI output is with the HAL (%s: %s)\n", TWL_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
	make_formats();
	return 0;
}

static OSStatus create_device(__unused AudioServerPlugInDriverRef d, __unused CFDictionaryRef c, __unused const AudioServerPlugInClientInfo *i,
    __unused AudioObjectID *o) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus destroy_device(__unused AudioServerPlugInDriverRef d, __unused AudioObjectID o) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus device_client(__unused AudioServerPlugInDriverRef d, __unused AudioObjectID o, __unused const AudioServerPlugInClientInfo *i) { return 0; }
static OSStatus perform_configuration_change(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 action, void *info);
static OSStatus abort_configuration_change(__unused AudioServerPlugInDriverRef d, __unused AudioObjectID o, __unused UInt64 a, __unused void *i) { return 0; }

struct value {
	UInt32 size;
	bool empty, string;
	union {
		UInt32 words[4];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged[8];
		uint8_t raw[12 + 20 * 8];       /* an AudioChannelLayout of up to 8 descriptions */
	} u;
};

static Float32 scalar_to_db(Float32 s) { s = s < 0 ? 0 : s > 1 ? 1 : s; return (Float32)(-48.0 * (1.0 - s)); }
static Float32 db_to_scalar(Float32 db) { Float32 s = (Float32)(1.0 + db / 48.0); return s < 0 ? 0 : s > 1 ? 1 : s; }

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
	if (object < OBJECT_PLUGIN || object > OBJECT_LAST) {
		return v;
	}
	if (object == OBJECT_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass: WORD(kAudioLevelControlClassID); break;
		case kAudioObjectPropertyClass: WORD(kAudioVolumeControlClassID); break;
		case kAudioObjectPropertyOwner: WORD(OBJECT_DEVICE); break;
		case kAudioObjectPropertyOwnedObjects: v.empty = true; break;
		case kAudioControlPropertyScope: WORD(kAudioObjectPropertyScopeOutput); break;
		case kAudioControlPropertyElement: WORD(0); break;
		case kAudioLevelControlPropertyScalarValue: v.u.decimal = volume; v.size = sizeof(Float32); break;
		case kAudioLevelControlPropertyDecibelValue: v.u.decimal = scalar_to_db(volume); v.size = sizeof(Float32); break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -48.0; v.u.range.mMaximum = 0; v.size = sizeof(AudioValueRange); break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar: v.u.decimal = 0; v.size = sizeof(Float32); break;
		}
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioObjectClassID); return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == OBJECT_DEVICE ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner: WORD(object == OBJECT_PLUGIN ? 0 : object == OBJECT_DEVICE ? OBJECT_PLUGIN : OBJECT_DEVICE); return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer: STRING(MANUFACTURER); break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList: LIST(OBJECT_DEVICE); break;
		case kAudioPlugInPropertyBoxList: v.empty = true; break;
		case kAudioPlugInPropertyTranslateUIDToBox: WORD(0); break;
		case kAudioPlugInPropertyTranslateUIDToDevice: WORD(0); break;       /* (filled in by get_property_data) */
		case kAudioPlugInPropertyResourceBundle: STRING(CFSTR("")); break;
		case 'clk#': v.empty = true; break;             /* no clock devices */
		case 'cust': v.empty = true; break;             /* no custom properties */
		}
	} else if (object == OBJECT_DEVICE) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer: STRING(MANUFACTURER); break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID: STRING(CFSTR(TWL_DEVICE_UID)); break;
		case kAudioDevicePropertyTransportType: WORD(TRANSPORT_HDMI); break;
		case kAudioDevicePropertyRelatedDevices: WORD(OBJECT_DEVICE); break;
		/* how far ahead of the play head the audio server writes: with none, a late I/O cycle left the hardware ring empty
		 * (an underrun a second, with a game playing); 1024 frames, 21 ms */
		case kAudioDevicePropertySafetyOffset: WORD(1024); break;
		/* the kernel starts the stream 60 ms ahead of the hardware (hakios.hdmiaudio.c, lead_ms): that is when what is written
		 * plays, which players need to know to keep the picture and the sound together */
		case kAudioDevicePropertyLatency: WORD(TWL_RATE * 60 / 1000); break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyIsHidden: WORD(0); break;
		case kAudioDevicePropertyDeviceIsAlive: WORD(1); break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: WORD(1); break;
		case kAudioDevicePropertyDeviceIsRunning: WORD(running != 0); break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			if (scope != kAudioObjectPropertyScopeInput) {
				LIST(OBJECT_STREAM);
			}
			if (selector == kAudioObjectPropertyOwnedObjects) {
				LIST(OBJECT_VOLUME);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList: LIST(OBJECT_VOLUME); break;
		case kAudioDevicePropertyNominalSampleRate: v.u.number = TWL_RATE; v.size = sizeof(Float64); break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.range.mMinimum = v.u.range.mMaximum = TWL_RATE; v.size = sizeof(AudioValueRange); break;
		case kAudioDevicePropertyPreferredChannelsForStereo: v.u.words[0] = 1; v.u.words[1] = 2; v.size = 2 * sizeof(UInt32); break;
		case kAudioDevicePropertyZeroTimeStampPeriod: WORD(TIMESTAMP_PERIOD); break;
		case 'cust': v.empty = true; break;
		case 'cfsz': WORD(512); break;                  /* kAudioDevicePropertyCalculateBufferFrameSize: (the qualifier's, below) */
		case 'oink': WORD((UInt32)hog_pid); break;      /* kAudioDevicePropertyHogMode: who has the device to itself, -1 nobody */
		case 'srnd': {                                  /* kAudioDevicePropertyPreferredChannelLayout: in the order of the HDMI slots (CTA-861 CA 0x0b, 0x13) */
			static const UInt32 labels[8] = { 1 /* L */, 2 /* R */, 4 /* LFE */, 3 /* C */, 5 /* Ls */, 6 /* Rs */, 33 /* Lrs */, 34 /* Rrs */ };
			UInt32 n = formats[current].kind == TWL_AUDIO_PCM ? formats[current].channels : 2, *w = (UInt32 *)v.u.raw;

			if (scope != kAudioObjectPropertyScopeOutput) {
				break;
			}
			if (n == 2) {
				w[0] = (101u << 16) | 2;                /* kAudioChannelLayoutTag_Stereo */
				w[1] = 0;
				w[2] = 0;
				v.size = 12;
			} else {
				w[0] = 0;                               /* kAudioChannelLayoutTag_UseChannelDescriptions */
				w[1] = 0;
				w[2] = n;
				for (UInt32 c = 0; c < n; c++) {
					w[3 + 5 * c] = labels[c];       /* label, flags 0, coordinates 0 */
					w[4 + 5 * c] = 0;
					w[5 + 5 * c] = w[6 + 5 * c] = w[7 + 5 * c] = 0;
				}
				v.size = 12 + 20 * n;
			}
			break;
		}
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive: WORD(!inactive); break;
		case kAudioStreamPropertyDirection: WORD(0); break;
		case kAudioStreamPropertyTerminalType: WORD(kAudioStreamTerminalTypeSpeaker); break;
		case kAudioStreamPropertyStartingChannel: WORD(1); break;
		case kAudioStreamPropertyLatency: WORD(0); break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_desc(&formats[current]);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			for (unsigned i = 0; i < format_count; i++) {
				v.u.ranged[i].mFormat = format_desc(&formats[i]);
				v.u.ranged[i].mSampleRateRange.mMinimum = v.u.ranged[i].mSampleRateRange.mMaximum = TWL_RATE;
			}
			v.size = format_count * sizeof(AudioStreamRangedDescription);
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
	return v;
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client, const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address);
	pthread_mutex_unlock(&lock);
	if (v.size == 0 && !v.empty) {
		say("twilight: audio: asked of object %u what it does not have: '%c%c%c%c', scope '%c%c%c%c'\n", (unsigned)object,
		    (int)(address->mSelector >> 24 & 0xff), (int)(address->mSelector >> 16 & 0xff), (int)(address->mSelector >> 8 & 0xff),
		    (int)(address->mSelector & 0xff), (int)(address->mScope >> 24 & 0xff), (int)(address->mScope >> 16 & 0xff),
		    (int)(address->mScope >> 8 & 0xff), (int)(address->mScope & 0xff));
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address,
    Boolean *settable)
{
	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue || address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_STREAM) {
		*settable = address->mSelector == kAudioStreamPropertyIsActive || address->mSelector == kAudioStreamPropertyVirtualFormat ||
		    address->mSelector == kAudioStreamPropertyPhysicalFormat;
	} else {
		*settable = object == OBJECT_DEVICE && (address->mSelector == kAudioDevicePropertyNominalSampleRate || address->mSelector == 'oink');
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address,
    __unused UInt32 qsize, __unused const void *q, UInt32 *size)
{
	struct value v;

	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address);
	pthread_mutex_unlock(&lock);
	*size = v.size;
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address,
    UInt32 qsize, const void *qualifier, UInt32 room, UInt32 *size, void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_VOLUME && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address);
	if (address->mSelector == 'cfsz' && qsize == sizeof(UInt32) && qualifier != NULL) {
		UInt32 want = *(const UInt32 *)qualifier;

		/* at least 512 frames (10.7 ms): a game asks for a few milliseconds, and at 32 frames the audio server ran some 1500 I/O
		 * cycles a second, mixing all of the game's voices each time -- two of the four cores, with Minecraft playing */
		v.u.words[0] = want < 512 ? 512 : want > TIMESTAMP_PERIOD ? TIMESTAMP_PERIOD : want;
	}
	if (object == OBJECT_PLUGIN && address->mSelector == kAudioPlugInPropertyTranslateUIDToDevice && qsize == sizeof(CFStringRef) &&
	    qualifier != NULL && *(const CFStringRef *)qualifier != NULL && CFEqual(*(const CFStringRef *)qualifier, CFSTR(TWL_DEVICE_UID))) {
		v.u.words[0] = OBJECT_DEVICE;
	}
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList && address->mSelector != 'srnd' && address->mSelector != kAudioStreamPropertyAvailablePhysicalFormats &&
		    address->mSelector != kAudioStreamPropertyAvailableVirtualFormats) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

/*
 * The device's timeline: the kernel's count of frames played starts again at 0 with each format it is given, and a
 * format change (Kodi's 5.1 after the menus' stereo) gave the audio server a sample time that went back to 0 under the
 * same seed while its I/O ran on: it then ran 85% of real time and Kodi's output timed out and was opened again, and
 * again. The count goes on from where it was instead (frame_base), and the seed changes with each restart.
 */
static UInt64 frame_base, last_hal_frames, anchor_whole = ~0ull, anchor_host;
static UInt32 clock_seed = 1;

/* The path in the kernel for the format now current (kind, channel count) */
static void
kernel_format(void)
{
	struct twl_audio_format f = { formats[current].kind, formats[current].hw_rate, formats[current].hw_channels, 16 };

	frame_base = last_hal_frames;
	anchor_whole = ~0ull;
	clock_seed++;

	if (sink >= 0 && ioctl(sink, TWL_AUDIO_SET_FORMAT, &f) < 0) {
		say("twilight: audio: the kernel would not take %u channels, kind %u (%s)\n", f.channels, f.kind, strerror(errno));
	} else {
		say("twilight: audio: format %u ch, kind %u, %u Hz to the kernel\n", f.channels, f.kind, f.rate);
	}
	have_position = true;
}

static void
volume_to_kernel(void)
{
	uint32_t gain = (uint32_t)(volume * 65536.0f);

	if (sink >= 0) {
		(void)ioctl(sink, TWL_AUDIO_SET_VOLUME, &gain);
	}
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address,
    __unused UInt32 qsize, __unused const void *q, UInt32 size, const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) : wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		volume = wanted;
		volume_to_kernel();
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_VOLUME, 2, changed);
		}
		return 0;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		inactive = size == sizeof(UInt32) && *(const UInt32 *)data == 0;
		return 0;
	}
	if (address->mSelector == 'oink') {
		if (size != sizeof(SInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		hog_pid = *(const SInt32 *)data;
		pthread_mutex_unlock(&lock);
		say("twilight: audio: hog mode %d\n", (int)hog_pid);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == TWL_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *w = data;

		say("twilight: audio: format asked for: '%c%c%c%c' flags %#x, %u ch, %u bit, %.0f Hz\n", (int)(w->mFormatID >> 24 & 0xff),
		    (int)(w->mFormatID >> 16 & 0xff), (int)(w->mFormatID >> 8 & 0xff), (int)(w->mFormatID & 0xff), (unsigned)w->mFormatFlags,
		    (unsigned)w->mChannelsPerFrame, (unsigned)w->mBitsPerChannel, w->mSampleRate);
		pthread_mutex_lock(&lock);
		for (unsigned i = 0; i < format_count; i++) {
			if (w->mFormatID == formats[i].id && w->mChannelsPerFrame == formats[i].channels && w->mSampleRate == TWL_RATE &&
			    w->mBitsPerChannel == 16) {
				/* through the host (it stops I/O and sizes its buffers for the new format, then calls
				 * perform_configuration_change): switched here directly, its buffers kept the old size, and a
				 * 2048-frame cycle of 6-channel PCM went out as 682 frames of sound and 1366 of silence (the 8 KiB
				 * of a stereo cycle), the "audio through a fan" of YouTube's 5.1 */
				pthread_mutex_unlock(&lock);
				if (i == current) {
					return 0;
				}
				if (host == NULL) {
					return kAudioHardwareIllegalOperationError;
				}
				return host->RequestDeviceConfigurationChange(host, OBJECT_DEVICE, i, NULL);
			}
		}
		pthread_mutex_unlock(&lock);
		return kAudioDeviceUnsupportedFormatError;
	}
}

/* the host's go-ahead for a change asked for above: the format whose index is `action' becomes the current one */
static OSStatus
perform_configuration_change(__unused AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 action, __unused void *info)
{
	AudioObjectPropertyAddress layout = { 'srnd', kAudioObjectPropertyScopeOutput, 0 };

	if (o != OBJECT_DEVICE || action >= format_count) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	current = (unsigned)action;
	if (running) {
		kernel_format();
	}
	pthread_mutex_unlock(&lock);
	say("twilight: audio: format %u (%u ch) by a configuration change\n", (unsigned)action, formats[action].channels);
	if (host != NULL) {
		host->PropertiesChanged(host, OBJECT_DEVICE, 1, &layout);
	}
	return 0;
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	if (object != OBJECT_DEVICE) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (running++ == 0) {
		anchor = mach_absolute_time();
		periods = 0;
		written = refused = 0;
		kernel_format();
		volume_to_kernel();
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	if (object != OBJECT_DEVICE) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (running != 0 && --running == 0) {
		if (sink >= 0) {
			(void)ioctl(sink, TWL_AUDIO_STOP);
		}
		have_position = false;
		say("twilight: audio: stopped after %llu frames (%llu not taken)\n", written, refused);
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * The device's clock. The kernel says how many frames the hardware has taken and when (TWL_AUDIO_GET_POSITION), which is the
 * audio clock the HDMI sink is locked to; the host's clock only paces it when that is not there.
 */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client, Float64 *sample_time,
    UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period = ticks_per_frame * TIMESTAMP_PERIOD;
	struct twl_audio_position pos;
	UInt64 now = mach_absolute_time();

	if (object != OBJECT_DEVICE) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (have_position && sink >= 0 && ioctl(sink, TWL_AUDIO_GET_POSITION, &pos) == 0 && pos.running) {
		UInt64 hal_frames = frame_base + pos.frames / formats[current].rate_div, whole = hal_frames / TIMESTAMP_PERIOD;

		last_hal_frames = hal_frames;

		/*
		 * A period's zero time stamp is worked out once and then given again: read afresh each call it moved by the DMA
		 * pointer's steps, the audio server took that for its clock jumping and skipped I/O cycles (a 512-frame cycle about
		 * every second, 1.2% of the stream: the ring ran dry every few seconds with a game playing)
		 */
		if (whole != anchor_whole) {
			anchor_host = pos.mach_time - (UInt64)((Float64)(hal_frames - whole * TIMESTAMP_PERIOD) * ticks_per_frame);
			anchor_whole = whole;
		}
		*sample_time = (Float64)(whole * TIMESTAMP_PERIOD);
		*host_time = anchor_host;
		*seed = clock_seed;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	while (now >= anchor + (UInt64)((Float64)(periods + 1) * ticks_per_period)) {
		periods++;
	}
	*sample_time = (Float64)periods * TIMESTAMP_PERIOD;
	*host_time = anchor + (UInt64)((Float64)periods * ticks_per_period);
	*seed = clock_seed;
	pthread_mutex_unlock(&lock);
	return 0;
}

/* touch /var/tmp/twlaudio-debug: every 2 s of I/O, what was written, what the hardware played and the kernel's counts,
 * to /var/tmp/twlaudio.log (the audio server's standard error goes nowhere) */
static void
io_log(UInt32 frames)
{
	static UInt64 since, cycles, frames_since, hw_at;
	UInt64 now = mach_absolute_time();
	struct twl_audio_position pos = { 0 };

	cycles++;
	frames_since += frames;
	if (since == 0) since = now;
	if ((Float64)(now - since) < 2.0 * TWL_RATE * ticks_per_frame) return;
	if (access("/var/tmp/twlaudio-debug", F_OK) == 0 && sink >= 0 && ioctl(sink, TWL_AUDIO_GET_POSITION, &pos) == 0) {
		FILE *f = fopen("/var/tmp/twlaudio.log", "a");
		if (f) {
			struct timespec t;
			clock_gettime(CLOCK_MONOTONIC, &t);
			fprintf(f, "%ld.%03ld: %llu cycles, %llu frames in %.2f s; hardware +%llu frames, fill %u, underruns %u, dropped %u, "
			    "refused %llu, running %u\n", (long)t.tv_sec, t.tv_nsec / 1000000, cycles, frames_since,
			    (Float64)(now - since) / ticks_per_frame / TWL_RATE, pos.frames - hw_at, pos.fill_frames, pos.underruns,
			    pos.dropped_frames, refused, pos.running);
			fclose(f);
		}
		hw_at = pos.frames;
	}
	since = now;
	cycles = frames_since = 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client, UInt32 operation,
    Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream, __unused UInt32 client,
    UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer, __unused void *secondary)
{
	if (stream != OBJECT_STREAM) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationWriteMix) {
		/* the kernel takes what fits (it never makes this thread wait) */
		if (sink < 0 || write(sink, buffer, (size_t)frames * formats[current].bytes_per_frame) < 0) {
			refused += frames;
		}
		written += frames;
		io_log(frames);
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	perform_configuration_change, abort_configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
TwlAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}
