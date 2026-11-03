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
		}
	}
	replace_method("SSDisplay", "setState:completion:", 0x1b3542ba8UL, (IMP)ss_display_set_state);
	replace_pb_method("PBApplicationSceneViewController", "_updateDeferralTargetAssertionIfNeeded", 0x10017a0acUL,
	    (IMP)update_deferral_target);
	original_did_update_readiness = (void (*)(obj_t, SEL, obj_t, bool))replace_pb_method("PBApplicationSceneViewController",
	    "applicationSceneView:didUpdateReadiness:", 0x10017a320UL, (IMP)did_update_readiness);
	replace_method("FBSDisplayConfiguration", "hardwareIdentifier", 0x187b1f034UL, (IMP)display_hardware_identifier);
	start_rechecks();
}
