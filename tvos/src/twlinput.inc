/*
 * Keys for tvOS: records of /dev/hakios-touch (HakiOS's queue; see sys/hakios.input.h) whose type is a button
 * and whose `finger` is (HID usage page << 16) | usage become keyboard events of a virtual HID service of ours
 * in backboardd's event server: the keyboard page (arrows, Return, Escape) or the consumer page (a remote's
 * Menu, Select, Up/Down/Left/Right, Play/Pause). Whoever writes them: the kernel's USB driver for the host's
 * keys, or tvos/src/keys.c replaying a list.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

struct hakios_touch { uint64_t time; uint32_t type, finger, x, y; };
#define HAKIOS_BUTTON_DOWN 4
#define HAKIOS_BUTTON_UP 5

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
struct virtual_service_callbacks {
	uint64_t version;
	void (*notification)(void *, void *, IOHIDServiceClientRef, uint32_t, CFDictionaryRef);
	Boolean (*setProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef, CFTypeRef);
	CFTypeRef (*copyProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef);
	IOHIDEventRef (*copyEvent)(void *, void *, IOHIDServiceClientRef, uint32_t, IOHIDEventRef, uint32_t);
	int (*setOutputEvent)(void *, void *, IOHIDServiceClientRef, IOHIDEventRef);
	CFTypeRef (*copyMatchingEvent)(void *, void *, IOHIDServiceClientRef, CFDictionaryRef);
};
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef, int, CFDictionaryRef);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef, dispatch_queue_t);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef, CFDictionaryRef,
    const struct virtual_service_callbacks *, void *, void *);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef, IOHIDEventRef);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef, uint64_t, uint32_t, uint32_t, Boolean, uint32_t);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef, uint32_t, CFIndex);
extern uint64_t mach_absolute_time(void);

struct service { CFMutableDictionaryRef properties; volatile bool opened; };

static CFTypeRef
svc_copy_property(void *target, void *refcon, IOHIDServiceClientRef s, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	(void)target, (void)s;
	return value ? CFRetain(value) : NULL;
}

static Boolean
svc_set_property(void *target, void *refcon, IOHIDServiceClientRef s, CFStringRef key, CFTypeRef value)
{
	(void)target, (void)s;
	if (key != NULL) {
		char name[96] = "?";
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("twl: input: the event server sets \"%s\" on %p\n", name, refcon);
	}
	if (key && value && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	return true;
}

static void
svc_notification(void *a, void *b, IOHIDServiceClientRef c, uint32_t d, CFDictionaryRef e)
{
	(void)a, (void)b, (void)c, (void)d, (void)e;
}

static const struct virtual_service_callbacks svc_callbacks = {
	.version = 2, .notification = svc_notification, .setProperty = svc_set_property, .copyProperty = svc_copy_property,
};

static CFNumberRef
num(int v)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &v);
}

static IOHIDServiceClientRef
make_keys(IOHIDEventSystemClientRef client, const char *product, int page, int usage)
{
	struct service *made = calloc(1, sizeof(*made));
	CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef pg = num(page), us = num(usage), zero = num(0);
	IOHIDServiceClientRef s = NULL;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), pg);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), us);
	CFArrayRef pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(p, CFSTR("PrimaryUsagePage"), pg);
	CFDictionarySetValue(p, CFSTR("PrimaryUsage"), us);
	CFDictionarySetValue(p, CFSTR("DeviceUsagePage"), pg);
	CFDictionarySetValue(p, CFSTR("DeviceUsage"), us);
	CFDictionarySetValue(p, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(p, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(p, CFSTR("Transport"), CFSTR("USB"));
	CFDictionarySetValue(p, CFSTR("Manufacturer"), CFSTR("twilight"));
	CFDictionarySetValue(p, CFSTR("Product"), CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8));
	CFDictionarySetValue(p, CFSTR("VendorID"), zero);
	CFDictionarySetValue(p, CFSTR("ProductID"), zero);
	made->properties = p;
	for (int attempt = 1; attempt <= 6; attempt++) {
		made->opened = false;
		s = IOHIDVirtualServiceClientCreateWithCallbacks(client, p, &svc_callbacks, NULL, made);
		if (s == NULL) {
			break;
		}
		for (int t = 0; t < 20 && !made->opened; t++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("twl: input: \"%s\" not opened by the event server (attempt %d)\n", product, attempt);
		IOHIDVirtualServiceClientRemove(s);
		CFRelease(s);
		s = NULL;
	}
	return s;
}

static void *
keys_thread(void *arg)
{
	IOHIDEventSystemClientRef client;
	IOHIDServiceClientRef keyboard, remote;
	struct hakios_touch t;
	unsigned count = 0;
	int device;

	(void)arg;
	pthread_setname_np("twilight keys");
	device = open("/dev/hakios-touch", O_RDONLY);
	if (device < 0) {
		say("twl: input: /dev/hakios-touch: %s\n", strerror(errno));
		return NULL;
	}
	sleep(3);
	keyboard = remote = NULL;
	/* a client of the event system that may make services: "simple" first, then the rate-controlled kind */
	for (int type = 4; type >= 3 && keyboard == NULL; type--) {
		client = IOHIDEventSystemClientCreateWithType(NULL, type, NULL);
		if (client == NULL) {
			say("twl: input: no HID client of type %d\n", type);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("twilight.keys", NULL));
		keyboard = make_keys(client, "twilight keyboard", 0x01, 0x06);  /* generic desktop: keyboard */
		if (keyboard != NULL) {
			remote = make_keys(client, "twilight remote", 0x0c, 0x01);      /* consumer control */
		} else {
			say("twl: input: a HID client of type %d may not make services\n", type);
		}
	}
	say("twl: input: keyboard %p, remote %p\n", (void *)keyboard, (void *)remote);
	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint32_t page = t.finger >> 16, usage = t.finger & 0xffff;
		IOHIDServiceClientRef s = page == 0x0c ? remote : keyboard;
		IOHIDEventRef e;

		if ((t.type != HAKIOS_BUTTON_DOWN && t.type != HAKIOS_BUTTON_UP) || page == 0 || s == NULL) {
			continue;
		}
		if (count++ < 50) {
			say("twl: input: key page %#x usage %#x %s\n", page, usage, t.type == HAKIOS_BUTTON_DOWN ? "down" : "up");
		}
		e = IOHIDEventCreateKeyboardEvent(NULL, mach_absolute_time(), page, usage, t.type == HAKIOS_BUTTON_DOWN, 0);
		IOHIDEventSetIntegerValue(e, 0x4 /* is built in */, 1);
		IOHIDVirtualServiceClientDispatchEvent(s, e);
		CFRelease(e);
	}
	say("twl: input: the device closed\n");
	return NULL;
}

static void
start_keys(void)
{
	pthread_t thread;

	pthread_create(&thread, NULL, keys_thread, NULL);
	pthread_detach(thread);
}
