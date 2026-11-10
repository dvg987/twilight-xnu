/*
 * libtwlpbdiag.dylib (injected into PineBoard): what holds applications inactive. Applications' scenes come with
 * deactivationReasons; this writes out PineBoard's side of it on request. The directory /var/tmp/twlpb must be
 * writable by PineBoard's user; a file there names the request:
 *   dump          the screensaver manager, the overlay layout coordinator, the critical alert manager, the scene
 *                 deactivation manager and the system overlay controller, as they describe themselves -> out
 *   cancelalert   the latest system alert (a CFUserNotification) cancelled, as Back would
 *   dialog        PBDialogManager's active dialog dismissed
 *   screensaver   the screensaver dismissed
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void *obj_t;
#define DIR "/var/tmp/twlpb/"

static FILE *out;

static obj_t
send0(obj_t o, const char *sel)
{
	if (o == NULL || !class_respondsToSelector(object_getClass(o), sel_registerName(sel))) {
		return NULL;
	}
	return ((obj_t (*)(obj_t, SEL))objc_msgSend)(o, sel_registerName(sel));
}

static void
put_object(const char *what, obj_t o)
{
	char buf[16384] = "(nil)";
	obj_t text = o ? send0(o, "description") : NULL;

	if (text != NULL) {
		CFStringGetCString((CFStringRef)text, buf, sizeof(buf), kCFStringEncodingUTF8);
	}
	fprintf(out, "== %s (%s):\n%s\n", what, o ? object_getClassName(o) : "-", buf);
	/* a TVSStateDumpProviding object says more in its state dump */
	obj_t dump = o ? send0(o, "stateDump") : NULL;
	if (dump == NULL && o != NULL) {
		obj_t builder = send0(o, "stateDumpBuilder");
		dump = builder ? send0(builder, "build") : NULL;
	}
	if (dump != NULL) {
		obj_t d = send0(dump, "description");
		if (d != NULL && CFStringGetCString((CFStringRef)d, buf, sizeof(buf), kCFStringEncodingUTF8)) {
			fprintf(out, "-- its state dump:\n%s\n", buf);
		}
	}
}

static obj_t
shared(const char *cls, const char *sel)
{
	Class c = objc_getClass(cls);
	return c ? send0((obj_t)c, sel) : NULL;
}

static void
dump_on_main(void *arg)
{
	(void)arg;
	out = fopen(DIR "out", "w");
	if (out == NULL) {
		return;
	}
	obj_t ss = shared("PBScreenSaverManager", "sharedInstance");
	fprintf(out, "screensaver presenting: %d\n", ss ? (int)((BOOL (*)(obj_t, SEL))objc_msgSend)(ss, sel_registerName("isPresenting")) : -1);
	put_object("PBScreenSaverManager", ss);
	put_object("PBOverlayLayoutCoordinator", shared("PBOverlayLayoutCoordinator", "defaultCoordinator"));
	obj_t dm = shared("PBDialogManager", "sharedInstance");
	fprintf(out, "dialog manager presenting: %d\n", dm ? (int)((BOOL (*)(obj_t, SEL))objc_msgSend)(dm, sel_registerName("isPresenting")) : -1);
	put_object("PBDialogManager.identifiersToContexts", send0(dm, "identifiersToContexts"));
	{
		obj_t contexts = send0(send0(dm, "identifiersToContexts"), "allValues");
		long n = contexts ? ((long (*)(obj_t, SEL))objc_msgSend)(contexts, sel_registerName("count")) : 0;
		for (long i = 0; i < n; i++) {
			obj_t c = ((obj_t (*)(obj_t, SEL, long))objc_msgSend)(contexts, sel_registerName("objectAtIndex:"), i);
			obj_t provider = send0(c, "provider");
			put_object("a dialog's provider", provider);
			put_object("  its view controller", send0(provider, "viewController"));
			put_object("  its reason", send0(c, "reason"));
		}
	}
	put_object("PBDialogManager.hiddenDialogAssertions", send0(dm, "hiddenDialogAssertions"));
	put_object("PBDialogManager.fullScreenOverlayOcclusionAssertion", send0(dm, "fullScreenOverlayOcclusionAssertion"));
	put_object("PBDialogManager.systemConfigurationDialogHiddenAssertion", send0(dm, "systemConfigurationDialogHiddenAssertion"));
	obj_t oc = send0(dm, "overlayController");
	put_object("PBDialogManager.overlayController", oc);
	put_object("PBDialogManager.overlayController.activeSession", send0(oc, "activeSession"));
	put_object("PBCriticalAlertManager", shared("PBCriticalAlertManager", "sharedInstance"));
	put_object("PBSystemOverlayController", shared("PBSystemOverlayController", "sharedInstance"));
	put_object("UIApplicationSceneDeactivationManager", shared("UIApplicationSceneDeactivationManager", "sharedInstance"));
	obj_t app = shared("UIApplication", "sharedApplication");
	put_object("the application's scene deactivation manager", send0(app, "_sceneDeactivationManager"));
	fclose(out);
}

static void
screensaver_on_main(void *arg)
{
	(void)arg;
	obj_t ss = shared("PBScreenSaverManager", "sharedInstance");
	if (ss != NULL) {
		((void (*)(obj_t, SEL))objc_msgSend)(ss, sel_registerName("dismissScreenSaver"));
	}
}

static void
dialog_on_main(void *arg)
{
	(void)arg;
	obj_t dm = shared("PBDialogManager", "sharedInstance");
	if (dm != NULL) {
		((void (*)(obj_t, SEL))objc_msgSend)(dm, sel_registerName("dismissActiveDialog"));
	}
}

/* who opens dialogs: each call written to /var/tmp/twlpb/log with its backtrace (image + offset per frame) */
static IMP orig_present, orig_block_init;

static void
log_backtrace(FILE *f)
{
	void *frames[24];
	int n = backtrace(frames, 24);
	for (int i = 1; i < n; i++) {
		Dl_info info = { 0 };
		if (dladdr(frames[i], &info) && info.dli_fname) {
			const char *base = strrchr(info.dli_fname, '/');
			fprintf(f, "    #%d %s + %#lx (%s)\n", i, base ? base + 1 : info.dli_fname, (unsigned long)((char *)frames[i] - (char *)info.dli_fbase),
			    info.dli_sname ? info.dli_sname : "?");
		}
	}
}

static void
describe(FILE *f, const char *what, obj_t o)
{
	char buf[2000] = "(nil)";
	obj_t t = o ? send0(o, "description") : NULL;
	if (t != NULL) {
		CFStringGetCString((CFStringRef)t, buf, sizeof(buf), kCFStringEncodingUTF8);
	}
	fprintf(f, "  %s: %s\n", what, buf);
}

static void
my_present(obj_t self, SEL sel, obj_t context, obj_t options, obj_t completion)
{
	FILE *f = fopen(DIR "log", "a");
	if (f != NULL) {
		fprintf(f, "presentDialogWithContext:\n");
		describe(f, "context", context);
		describe(f, "identifier", send0(context, "identifier"));
		describe(f, "reason", send0(context, "reason"));
		describe(f, "options", options);
		log_backtrace(f);
		fclose(f);
	}
	((void (*)(obj_t, SEL, obj_t, obj_t, obj_t))orig_present)(self, sel, context, options, completion);
}

static obj_t
my_block_init(obj_t self, SEL sel, obj_t block)
{
	FILE *f = fopen(DIR "log", "a");
	if (f != NULL) {
		void *fn = block ? *(void **)((char *)block + 16) : NULL;
		Dl_info info = { 0 };
		dladdr(fn, &info);
		fprintf(f, "PBBlockSystemOverlayUIProvider initWithBlock: %p = %s + %#lx\n", fn, info.dli_fname ? info.dli_fname : "?",
		    (unsigned long)((char *)fn - (char *)info.dli_fbase));
		log_backtrace(f);
		fclose(f);
	}
	return ((obj_t (*)(obj_t, SEL, obj_t))orig_block_init)(self, sel, block);
}

static IMP orig_cfnote;
static obj_t last_handler, notes[16];  /* the system alerts shown, to cancel on request */
static unsigned nnotes;

/* a CFUserNotification some process posted (a system modal alert): what it says, and everything else it carries */
static void
my_cfnote(obj_t self, SEL sel, obj_t note)
{
	FILE *f = fopen(DIR "log", "a");
	if (f != NULL) {
		fprintf(f, "userNotificationCenterDidReceiveCFNotification: %s\n", note ? object_getClassName(note) : "-");
		describe(f, "notification", note);
		static const char *const props[] = { "contents", "dictionary", "alertHeader", "alertMessage", "header", "message",
		                                     "clientPID", "pid", "processIdentifier", "bundleIdentifier", "sourceBundleIdentifier",
		                                     "identifier", "flags" };
		for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
			if (note && class_respondsToSelector(object_getClass(note), sel_registerName(props[i]))) {
				describe(f, props[i], send0(note, props[i]));
			}
		}
		fclose(f);
	}
	/* (unless /var/tmp/twlpb/show-appleid exists) an Apple ID alert (akd, appstored: "Apple ID Required", "Sign in with Apple ID") is neither
	 * shown nor answered -- its poster waits; cancelled, akd posts it again at once -- so it cannot hold every application inactive */
	{
		obj_t header = send0(note, "alertHeader");
		char text[200] = "";
		if (header != NULL) {
			CFStringGetCString((CFStringRef)header, text, sizeof(text), kCFStringEncodingUTF8);
		}
		/* ("Apple ID" is written with a no-break space) */
		if (strstr(text, "Apple") != NULL && strstr(text, "ID") != NULL && access(DIR "show-appleid", F_OK) != 0) {
			f = fopen(DIR "log", "a");
			if (f != NULL) {
				fprintf(f, "  held back (not shown, not answered): %s\n", text);
				fclose(f);
			}
			((void (*)(obj_t, SEL))objc_msgSend)(note, sel_registerName("retain"));
			return;
		}
	}
	last_handler = self;
	if (nnotes < 16) {
		notes[nnotes++] = note;
	}
	((void (*)(obj_t, SEL))objc_msgSend)(note, sel_registerName("retain"));
	((void (*)(obj_t, SEL, obj_t))orig_cfnote)(self, sel, note);
}

/* as if Back were pressed on it: the poster is told it was cancelled, the dialog goes */
static void
cancel_alert_on_main(void *arg)
{
	(void)arg;
	FILE *f = fopen(DIR "log", "a");
	for (unsigned i = 0; i < nnotes; i++) {
		obj_t note = notes[i];
		((void (*)(obj_t, SEL))objc_msgSend)(note, sel_registerName("cancel"));
		if (last_handler && class_respondsToSelector(object_getClass(last_handler), sel_registerName("userNotificationCenterDidCancelCFNotification:"))) {
			((void (*)(obj_t, SEL, obj_t))objc_msgSend)(last_handler, sel_registerName("userNotificationCenterDidCancelCFNotification:"), note);
		}
		if (f) { fprintf(f, "cancelalert: %p cancelled\n", note); }
	}
	nnotes = 0;
	if (f) { fclose(f); }
}

static void
hook(const char *cls, const char *sel, IMP replacement, IMP *original)
{
	Class c = objc_getClass(cls);
	Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
	if (m != NULL) {
		*original = method_setImplementation(m, replacement);
	}
}

static void *
watch(void *arg)
{
	(void)arg;
	for (;;) {
		if (access(DIR "dump", F_OK) == 0) {
			unlink(DIR "dump");
			dispatch_async_f(dispatch_get_main_queue(), NULL, dump_on_main);
		}
		if (access(DIR "cancelalert", F_OK) == 0) {
			unlink(DIR "cancelalert");
			dispatch_async_f(dispatch_get_main_queue(), NULL, cancel_alert_on_main);
		}
		if (access(DIR "dialog", F_OK) == 0) {
			unlink(DIR "dialog");
			dispatch_async_f(dispatch_get_main_queue(), NULL, dialog_on_main);
		}
		if (access(DIR "screensaver", F_OK) == 0) {
			unlink(DIR "screensaver");
			dispatch_async_f(dispatch_get_main_queue(), NULL, screensaver_on_main);
		}
		sleep(1);
	}
	return NULL;
}

__attribute__((constructor)) static void
twlpbdiag_init(void)
{
	pthread_t t;

	hook("PBDialogManager", "presentDialogWithContext:options:completion:", (IMP)my_present, &orig_present);
	hook("PBUserNotificationHandler", "userNotificationCenterDidReceiveCFNotification:", (IMP)my_cfnote, &orig_cfnote);
	hook("PBBlockSystemOverlayUIProvider", "initWithBlock:", (IMP)my_block_init, &orig_block_init);

	if (pthread_create(&t, NULL, watch, NULL) == 0) {
		pthread_detach(t);
	}
}
