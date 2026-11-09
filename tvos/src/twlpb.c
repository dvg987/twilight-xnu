/*
 * libtwlpb.dylib: inside tvOS's PineBoard. The display is woken and put to sleep through ATVSystemState's
 * SSDisplay, which drives Apple's HDMI controller (IOAVController); this box has none, so every
 * -[SSDisplay setState:completion:] fails, PineBoard's "WakeComplete" latch never closes and its main thread
 * spends its life waiting for it ("Waiting for latches to close... timed out"), with no time left for input.
 * Here the HDMI output is always on: the state change succeeds at once.
 *
 * Keys go to backboardd's keyboard focus, which PineBoard hands to the application on screen (HeadBoard,
 * the home screen) once its own focus engine has focused that application's scene view; with no display
 * that PineBoard believes in, that never happens and every key stays in PineBoard. Here a scene that
 * exists is the deferral target, focused or not. PineBoard looks again only when something it watches
 * changes, and the first looks come while the boot's black overlay still covers the scene ("deactivation
 * reasons: occluded"); so it is asked again every two seconds. It also defers only to a display with a
 * hardware identifier ("Deferrals unchanged, no display"), which CoreAnimation's software display has not got:
 * the main display gets one here.
 * Addresses are tvOS 16.5 (20L563) AppleTV6,2's, unslid; each is checked first.
 */
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern void objc_release(void *);

static uintptr_t slide;       /* of the shared cache */
static uintptr_t pb_slide;    /* of PineBoard itself */

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
	{
		/* (a copy for ssh: the console's goes only to the run's log) */
		static int fd = -2;

		if (fd == -2) {
			fd = open("/var/tmp/twl-pb.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
		}
		if (fd >= 0) {
			write(fd, line, (size_t)n);
		}
	}
}

static IMP
replace_method_at(const char *class_name, const char *selector, uintptr_t expected, uintptr_t s, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + s) {
		say("twl: -[%s %s]: found %#lx, expected %#lx; left alone\n", class_name, selector,
		    original ? (uintptr_t)original - s : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

#define replace_method(c, s, e, r) replace_method_at(c, s, e, slide, r)
#define replace_pb_method(c, s, e, r) replace_method_at(c, s, e, pb_slide, r)

typedef void *obj_t;
/* objc_msgSend through its real prototype each time: arm64 passes variadic arguments on the stack */
#define SEND0(type, obj, sel) ((type (*)(obj_t, SEL))objc_msgSend)((obj), sel_registerName(sel))
#define SEND2(type, obj, sel, a, b) ((type (*)(obj_t, SEL, obj_t, obj_t))objc_msgSend)((obj), sel_registerName(sel), (a), (b))

static obj_t *
ivar_slot(obj_t self, const char *name)
{
	Ivar ivar = class_getInstanceVariable(object_getClass((id)self), name);

	return ivar ? (obj_t *)((char *)self + ivar_getOffset(ivar)) : NULL;
}

/* -[PBApplicationSceneViewController _updateDeferralTargetAssertionIfNeeded], without asking the focus engine */
static void
update_deferral_target(obj_t self, SEL selector)
{
	obj_t *handle = ivar_slot(self, "_sceneHandle"), *assertion = ivar_slot(self, "_deferralTargetAssertion");
	obj_t scene;

	(void)selector;
	if (handle == NULL || assertion == NULL || *handle == NULL) {
		return;
	}
	scene = SEND0(obj_t, *handle, "sceneIfExists");
	if (scene == NULL) {
		if (*assertion != NULL) {
			SEND0(void, *assertion, "invalidate");
			objc_release(*assertion);
			*assertion = NULL;
		}
		return;
	}
	if (*assertion != NULL && SEND0(bool, *assertion, "isValid")) {
		return;
	}
	if (*assertion != NULL) {
		objc_release(*assertion);
	}
	*assertion = SEND2(obj_t, SEND0(obj_t, (obj_t)objc_getClass("PBSceneDeferralTargetAssertion"), "alloc"),
	    "initWithSceneHandle:reason:", *handle, (obj_t)CFSTR("Focus Deferral"));
	say("twl: keys now go to scene %p (%s)\n", scene, object_getClassName((id)scene));
}

/* -[PBApplicationSceneViewController applicationSceneView:didUpdateReadiness:]: then take the keys */
static void (*original_did_update_readiness)(obj_t, SEL, obj_t, bool);

static void
did_update_readiness(obj_t self, SEL selector, obj_t view, bool ready)
{
	if (original_did_update_readiness != NULL) {
		original_did_update_readiness(self, selector, view, ready);
	}
	update_deferral_target(self, NULL);
}

/* -[SSDisplay setState:completion:]: the completion takes (BOOL succeeded, NSError *error) */
static void
ss_display_set_state(void *self, SEL selector, long state, void (^completion)(bool, void *))
{
	(void)self, (void)selector;
	say("twl: SSDisplay setState %ld: done (no HDMI controller here)\n", state);
	if (completion != NULL) {
		void (^done)(bool, void *) = (void (^)(bool, void *))_Block_copy(completion);

		dispatch_async(dispatch_get_global_queue(0, 0), ^{
			done(true, NULL);
			_Block_release(done);
		});
	}
}

/* -[FBSDisplayConfiguration hardwareIdentifier] */
static obj_t
display_hardware_identifier(obj_t self, SEL selector)
{
	obj_t *id = ivar_slot(self, "_hardwareIdentifier");

	(void)selector;
	return id != NULL && *id != NULL ? *id : (obj_t)CFSTR("twilight-hdmi");
}

static void
recheck_deferrals(void *unused)
{
	obj_t app = SEND0(obj_t, (obj_t)objc_getClass("UIApplication"), "sharedApplication");
	obj_t *manager = app ? ivar_slot(app, "_sceneManager") : NULL;

	(void)unused;
	if (manager != NULL && *manager != NULL) {
		SEND0(void, *manager, "_updateFocusDeferralsIfNeeded");
	}
}

static void
start_rechecks(void)
{
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 2 * NSEC_PER_SEC,
	    NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler_f(timer, recheck_deferrals);
	dispatch_resume(timer);
}

/*
 * Which applications the home screen shows: PineBoard's app-info policy asks -isApplicationEnabled:reasons: (and
 * -isPlaceholderEnabled:reasons:) of each, and without an activation record, a storefront or Screen Time's policy
 * daemon only Settings, Photos and Computers pass. Here each answer and its reasons (a bit mask, PBSAppInfo's
 * disabledReasons) are logged once per application, and every application is enabled. -isApplicationHidden: is only
 * logged: what Apple hides (What's New, the services) stays hidden.
 */
#define ENABLE_HOOKS 4
static struct { Class cls; SEL sel; IMP original; } enable_hooks[ENABLE_HOOKS];
static unsigned enable_hook_count;
static char enable_seen[64][96];
static unsigned enable_seen_count;

static IMP
enable_original(obj_t self, SEL sel)
{
	for (Class c = object_getClass((id)self); c != NULL; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < enable_hook_count; i++) {
			if (enable_hooks[i].cls == c && enable_hooks[i].sel == sel) {
				return enable_hooks[i].original;
			}
		}
	}
	return NULL;
}

static void
app_name(obj_t app, char *out, size_t size)
{
	obj_t name = NULL;

	out[0] = 0;
	if (app == NULL) {
		strlcpy(out, "(nil)", size);
		return;
	}
	if (((bool (*)(obj_t, SEL, SEL))objc_msgSend)(app, sel_registerName("respondsToSelector:"),
	    sel_registerName("bundleIdentifier"))) {
		name = SEND0(obj_t, app, "bundleIdentifier");
	} else if (CFGetTypeID((CFTypeRef)app) == CFStringGetTypeID()) {
		name = app;
	}
	if (name == NULL || !CFStringGetCString((CFStringRef)name, out, (CFIndex)size, kCFStringEncodingUTF8)) {
		snprintf(out, size, "<%s>", object_getClassName((id)app));
	}
}

static bool
enable_first_time(const char *what, const char *name)
{
	char key[96];

	snprintf(key, sizeof(key), "%s %s", what, name);
	for (unsigned i = 0; i < enable_seen_count; i++) {
		if (strcmp(enable_seen[i], key) == 0) {
			return false;
		}
	}
	if (enable_seen_count < 64) {
		strlcpy(enable_seen[enable_seen_count++], key, sizeof(enable_seen[0]));
	}
	return true;
}

static bool
is_enabled(obj_t self, SEL sel, obj_t app, uint64_t *reasons)
{
	bool (*original)(obj_t, SEL, obj_t, uint64_t *) = (bool (*)(obj_t, SEL, obj_t, uint64_t *))enable_original(self, sel);
	uint64_t why = 0;
	bool on = original != NULL ? original(self, sel, app, &why) : true;
	char name[80];

	app_name(app, name, sizeof(name));
	if (enable_first_time(sel_getName(sel), name)) {
		say("twl: %s %s: %s, reasons %#llx%s\n", sel_getName(sel), name, on ? "enabled" : "disabled",
		    (unsigned long long)why, on ? "" : "; enabled here");
	}
	if (reasons != NULL) {
		*reasons = 0;
	}
	return true;
}

static bool
is_hidden(obj_t self, SEL sel, obj_t app)
{
	bool (*original)(obj_t, SEL, obj_t) = (bool (*)(obj_t, SEL, obj_t))enable_original(self, sel);
	bool hidden = original != NULL ? original(self, sel, app) : false;
	char name[80];

	app_name(app, name, sizeof(name));
	if (enable_first_time("hidden", name)) {
		say("twl: isApplicationHidden: %s: %s\n", name, hidden ? "hidden" : "shown");
	}
	return hidden;
}

/* every class of PineBoard's own that implements SELECTOR itself */
static void
hook_pb_selector(const char *image, const char *selector, IMP replacement)
{
	unsigned count = 0;
	const char **names = objc_copyClassNamesForImage(image, &count);
	SEL sel = sel_registerName(selector);

	for (unsigned i = 0; names != NULL && i < count; i++) {
		Class cls = objc_getClass(names[i]);
		unsigned n = 0;
		Method *methods = cls != NULL ? class_copyMethodList(cls, &n) : NULL;

		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) == sel && enable_hook_count < ENABLE_HOOKS) {
				enable_hooks[enable_hook_count].cls = cls;
				enable_hooks[enable_hook_count].sel = sel;
				enable_hooks[enable_hook_count].original = method_setImplementation(methods[k], replacement);
				enable_hook_count++;
				say("twl: -[%s %s] hooked\n", names[i], selector);
			}
		}
		free(methods);
	}
	free(names);
}

/*
 * A TV out (kern.fb_external, the default): backboardd draws a 1920x1080 canvas in every mode (tvos/src/twlhook.c), so a
 * mode taller than 1080 lines is scale 1 (CoreAnimation says 2): the apps' screens are 1920x1080 points.
 */
static unsigned long (*mode_scale_o)(obj_t, SEL);

static unsigned long
mode_scale(obj_t self, SEL sel)
{
	unsigned long r = mode_scale_o(self, sel);

	return r > 1 && SEND0(unsigned long, self, "height") > 1080 ? 1 : r;
}

static void
tv_out_scale(void)
{
	uint32_t external = 0;
	size_t size = sizeof(external);

	if (sysctlbyname("kern.fb_external", &external, &size, NULL, 0) != 0 || external == 0 || access("/var/mobile/twl/nocanvas", F_OK) == 0) {
		return;
	}
	mode_scale_o = (unsigned long (*)(obj_t, SEL))replace_method("CADisplayMode", "preferredScale", 0x1899ce110UL, (IMP)mode_scale);
	if (mode_scale_o != NULL) {
		say("twl: TV out: modes at scale 1\n");
	}
}

/* (tracing the remote's Menu through PineBoard: which classes implement these, and what they answer) */
#define TRACE_HOOKS 16
static struct { Class cls; SEL sel; IMP original; } trace_hooks[TRACE_HOOKS];
static unsigned trace_hook_count;

static IMP
trace_original(obj_t self, SEL sel)
{
	for (Class c = object_getClass((id)self); c != NULL; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < trace_hook_count; i++) {
			if (trace_hooks[i].cls == c && trace_hooks[i].sel == sel) {
				return trace_hooks[i].original;
			}
		}
	}
	return NULL;
}

static long
trace0(obj_t self, SEL sel)
{
	long r = ((long (*)(obj_t, SEL))trace_original(self, sel))(self, sel);

	say("twl: trace -[%s %s] -> %ld\n", object_getClassName((id)self), sel_getName(sel), r);
	return r;
}

static long
trace1(obj_t self, SEL sel, obj_t a)
{
	long r;

	say("twl: trace -[%s %s] %p ...\n", object_getClassName((id)self), sel_getName(sel), a);
	r = ((long (*)(obj_t, SEL, obj_t))trace_original(self, sel))(self, sel, a);
	say("twl: trace -[%s %s] -> %ld\n", object_getClassName((id)self), sel_getName(sel), r);
	return r;
}

static long
trace2(obj_t self, SEL sel, long a, obj_t b)
{
	long r;

	say("twl: trace -[%s %s] %ld %p ...\n", object_getClassName((id)self), sel_getName(sel), a, b);
	r = ((long (*)(obj_t, SEL, long, obj_t))trace_original(self, sel))(self, sel, a, b);
	say("twl: trace -[%s %s] -> %ld\n", object_getClassName((id)self), sel_getName(sel), r);
	return r;
}

static long
trace3(obj_t self, SEL sel, obj_t a, obj_t b, obj_t c)
{
	say("twl: trace -[%s %s] (going Home?)\n", object_getClassName((id)self), sel_getName(sel));
	return ((long (*)(obj_t, SEL, obj_t, obj_t, obj_t))trace_original(self, sel))(self, sel, a, b, c);
}

static void
trace_selector(const char *image, const char *selector, IMP replacement)
{
	unsigned count = 0;
	const char **names = objc_copyClassNamesForImage(image, &count);
	SEL sel = sel_registerName(selector);

	for (unsigned i = 0; names != NULL && i < count; i++) {
		Class cls = objc_getClass(names[i]);
		unsigned n = 0;
		Method *methods = cls != NULL ? class_copyMethodList(cls, &n) : NULL;

		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) == sel && trace_hook_count < TRACE_HOOKS) {
				trace_hooks[trace_hook_count].cls = cls;
				trace_hooks[trace_hook_count].sel = sel;
				trace_hooks[trace_hook_count].original = method_setImplementation(methods[k], replacement);
				trace_hook_count++;
				say("twl: tracing -[%s %s]\n", names[i], selector);
			}
		}
		free(methods);
	}
	free(names);
}

/*
 * (touch /var/mobile/twl/trace-modes: match-content's choices -- every PineBoard method about frame rates, display modes
 * and match content, with its arguments and answer. arm64 passes integers and pointers in x2.. and doubles in d0..
 * apart, so one hook of (x2, x3, x4, d0, d1) passes any mix of up to three of each through unchanged.)
 */
#define MODE_HOOKS 96
static struct { Class cls; SEL sel; IMP original; char types[64]; unsigned told; } mode_hooks[MODE_HOOKS];
static unsigned mode_hook_count;

static int
mode_hook(obj_t self, SEL sel)
{
	for (Class c = object_getClass((id)self); c != NULL; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < mode_hook_count; i++) {
			if (mode_hooks[i].cls == c && mode_hooks[i].sel == sel) {
				return (int)i;
			}
		}
	}
	return -1;
}

static void
describe(char *out, size_t size, char type, long x, double d)
{
	if (type == '@') {
		obj_t s = x ? SEND0(obj_t, (obj_t)x, "description") : NULL;
		const char *u = s ? SEND0(const char *, s, "UTF8String") : NULL;
		snprintf(out, size, "%s", u ? u : "nil");
		for (char *p = out; *p; p++) if (*p == '\n') *p = ' ';
	} else if (type == 'd' || type == 'f') {
		snprintf(out, size, "%.3f", d);
	} else {
		snprintf(out, size, "%ld", x);
	}
}

static void
mode_say(int h, obj_t self, SEL sel, const long *x, const double *d, long rx, double rd)
{
	const char *t = mode_hooks[h].types;   /* return type, then the arguments' (after self and _cmd) */
	char line[1200], part[400];
	unsigned xi = 0, di = 0;
	size_t at = 0;

	if (mode_hooks[h].told++ >= 40) return;
	at += (size_t)snprintf(line + at, sizeof(line) - at, "twl: mode -[%s %s]", object_getClassName((id)self), sel_getName(sel));
	for (const char *a = t + 1; *a && at < sizeof(line) - 2; a++) {
		bool fp = *a == 'd' || *a == 'f';
		describe(part, sizeof(part), *a, fp ? 0 : x[xi < 3 ? xi : 2], fp ? d[di < 2 ? di : 1] : 0);
		fp ? di++ : xi++;
		at += (size_t)snprintf(line + at, sizeof(line) - at, " (%s)", part);
	}
	if (t[0] != 'v' && at < sizeof(line) - 2) {
		describe(part, sizeof(part), t[0], rx, rd);
		at += (size_t)snprintf(line + at, sizeof(line) - at, " -> %s", part);
	}
	say("%.*s\n", (int)(at < sizeof(line) ? at : sizeof(line) - 1), line);
}

static long
mode_x(obj_t self, SEL sel, long a, long b, long c, double d0, double d1)
{
	int h = mode_hook(self, sel);
	long x[3] = { a, b, c };
	double d[2] = { d0, d1 };
	long r = ((long (*)(obj_t, SEL, long, long, long, double, double))mode_hooks[h].original)(self, sel, a, b, c, d0, d1);

	mode_say(h, self, sel, x, d, r, 0);
	return r;
}

static double
mode_d(obj_t self, SEL sel, long a, long b, long c, double d0, double d1)
{
	int h = mode_hook(self, sel);
	long x[3] = { a, b, c };
	double d[2] = { d0, d1 };
	double r = ((double (*)(obj_t, SEL, long, long, long, double, double))mode_hooks[h].original)(self, sel, a, b, c, d0, d1);

	mode_say(h, self, sel, x, d, 0, r);
	return r;
}

static void
trace_modes(const char *image)
{
	static const char *const words[] = { "rameRate", "atchContent", "DisplayMode", "isplayCriteria", "RefreshRate",
	    "DynamicRange", "dynamicRange", NULL };
	unsigned count = 0;
	const char **names = objc_copyClassNamesForImage(image, &count);

	for (unsigned i = 0; names != NULL && i < count; i++) {
		Class cls = objc_getClass(names[i]);
		unsigned n = 0;
		Method *methods = cls != NULL ? class_copyMethodList(cls, &n) : NULL;

		for (unsigned k = 0; k < n && mode_hook_count < MODE_HOOKS; k++) {
			const char *name = sel_getName(method_getName(methods[k]));
			bool want = false;
			char types[64];
			unsigned args = method_getNumberOfArguments(methods[k]);
			unsigned xs = 0, ds = 0;

			for (unsigned w = 0; words[w] != NULL; w++) want = want || strstr(name, words[w]) != NULL;
			if (!want) continue;
			method_getReturnType(methods[k], types, 2);
			if (strchr("@cBqQiIlLsSvdf", types[0]) == NULL || args > 2 + 5) continue;
			bool ok = true;
			for (unsigned a = 2; a < args && ok; a++) {
				char at[8];
				method_getArgumentType(methods[k], a, at, sizeof(at));
				if (strchr("@cBqQiIlLsS:#", at[0]) != NULL) { xs++; types[a - 1] = at[0]; }
				else if (at[0] == 'd' || at[0] == 'f') { ds++; types[a - 1] = at[0]; }
				else ok = false;
			}
			if (!ok || xs > 3 || ds > 2) continue;
			types[args - 1] = 0;
			mode_hooks[mode_hook_count].cls = cls;
			mode_hooks[mode_hook_count].sel = method_getName(methods[k]);
			memcpy(mode_hooks[mode_hook_count].types, types, sizeof(types));
			mode_hooks[mode_hook_count].original = method_setImplementation(methods[k],
			    types[0] == 'd' || types[0] == 'f' ? (IMP)mode_d : (IMP)mode_x);
			mode_hook_count++;
		}
		free(methods);
	}
	free(names);
	say("twl: tracing %u methods about modes\n", mode_hook_count);
}

__attribute__((constructor)) static void
twl_pb_init(void)
{
	char path[1024];
	uint32_t size = sizeof(path);
	size_t length;

	if (_NSGetExecutablePath(path, &size) != 0 || strstr(path, "PineBoard") == NULL) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	dlopen("/System/Library/PrivateFrameworks/ATVSystemState.framework/ATVSystemState", RTLD_NOW);
	say("twl: in %s, shared cache slide %#lx\n", path, slide);
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "PineBoard.app/PineBoard") != NULL) {
			pb_slide = (uintptr_t)_dyld_get_image_vmaddr_slide(i);
			hook_pb_selector(name, "isApplicationEnabled:reasons:", (IMP)is_enabled);
			hook_pb_selector(name, "isPlaceholderEnabled:reasons:", (IMP)is_enabled);
			hook_pb_selector(name, "isApplicationHidden:", (IMP)is_hidden);
			if (access("/var/mobile/twl/trace-menu", F_OK) == 0) {
				trace_selector(name, "handleBackButton", (IMP)trace0);
				trace_selector(name, "handleMenuSingleTapEvent:", (IMP)trace1);
				trace_selector(name, "handleUnhandledMenuEvent", (IMP)trace0);
				trace_selector(name, "_handleMenuOrACHomeSingleTapEventWithYesForMenuNoForACHome:recognizer:", (IMP)trace2);
				trace_selector(name, "shouldForwardSystemAppEvent", (IMP)trace0);
				trace_selector(name, "openKioskApplicationWithReason:options:completion:", (IMP)trace3);
				trace_selector(name, "_checkForBulletinActivationForAction:", (IMP)trace1);
			}
			if (access("/var/mobile/twl/trace-modes", F_OK) == 0) {
				trace_modes(name);
			}
		}
	}
	replace_method("SSDisplay", "setState:completion:", 0x1b3542ba8UL, (IMP)ss_display_set_state);
	replace_pb_method("PBApplicationSceneViewController", "_updateDeferralTargetAssertionIfNeeded", 0x10017a0acUL,
	    (IMP)update_deferral_target);
	original_did_update_readiness = (void (*)(obj_t, SEL, obj_t, bool))replace_pb_method("PBApplicationSceneViewController",
	    "applicationSceneView:didUpdateReadiness:", 0x10017a320UL, (IMP)did_update_readiness);
	replace_method("FBSDisplayConfiguration", "hardwareIdentifier", 0x187b1f034UL, (IMP)display_hardware_identifier);
	tv_out_scale();
	start_rechecks();
}
