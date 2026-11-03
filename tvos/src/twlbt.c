/*
 * libtwlbt.dylib: inside tvOS's Settings. A Bluetooth LE accessory that pairs "Just Works" (no display, no keys: the box's
 * own remote) has bluetoothd ask the app's pairing agent to confirm, and Settings is to put up an alert; here that alert
 * never comes up, the app waits for an answer, and the remote gives up a few seconds later (SMP status 719). So a
 * Just Works request is accepted at once, for the user; any other kind (a passkey, a number to compare) still goes to
 * Settings. Addresses are tvOS 16.5 (20L563) AppleTV6,2's, unslid; each is checked first.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

typedef void *obj_t;

#define SEND0(type, obj, sel) ((type (*)(obj_t, SEL))objc_msgSend)((obj), sel_registerName(sel))
#define SEND1(type, obj, sel, a) ((type (*)(obj_t, SEL, obj_t))objc_msgSend)((obj), sel_registerName(sel), (a))

#define PAIRING_JUST_WORKS 0                    /* CBPairingType: Just Works, Passkey Display, Passkey Entry, Numeric
                                                 * Comparison, Out-of-band (bluetoothd's names, in that order) */

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[300];
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

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("twl: -[%s %s]: found %#lx, expected %#lx; left alone\n", class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

/* -[CBPairingAgent handlePairingRequested:]: its argument the message's dictionary (peer, type, passkey) */
static void (*original_pairing_requested)(obj_t, SEL, obj_t);

static void
pairing_requested(obj_t self, SEL selector, obj_t args)
{
	obj_t number = SEND1(obj_t, args, "objectForKeyedSubscript:", (obj_t)CFSTR("kCBMsgArgPairingType"));
	long type = number != NULL ? SEND0(long, number, "integerValue") : -1;

	if (type == PAIRING_JUST_WORKS) {
		obj_t manager = SEND0(obj_t, self, "parentManager");
		obj_t peer = manager != NULL ? SEND1(obj_t, manager, "peerWithInfo:", args) : NULL;

		if (peer != NULL) {
			say("twl: Bluetooth: a Just Works pairing request: accepted\n");
			((void (*)(obj_t, SEL, obj_t, long, bool, obj_t))objc_msgSend)(self,
			    sel_registerName("respondToPairingRequest:type:accept:data:"), peer, type, true, NULL);
			return;
		}
	}
	say("twl: Bluetooth: a pairing request of type %ld: for Settings\n", type);
	original_pairing_requested(self, selector, args);
}

__attribute__((constructor)) static void
twl_bt_init(void)
{
	char path[1024];
	uint32_t size = sizeof(path);
	size_t length;

	if (_NSGetExecutablePath(path, &size) != 0 || strstr(path, "TVSettings") == NULL) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", RTLD_NOW);
	original_pairing_requested = (void (*)(obj_t, SEL, obj_t))replace_method("CBPairingAgent", "handlePairingRequested:",
	    0x18b5af384UL, (IMP)pairing_requested);
}
