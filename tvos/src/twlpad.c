/*
 * libtwlpad.dylib: a virtual game controller inside an application, driven from a shell -- for trying games that
 * want an extended gamepad (Minecraft's Apple TV edition will not start without one) with nobody at the TV.
 *
 * Injected into the program (kern.exec_inject / etc/inject.conf), it makes a controller with GameController's own
 * +[GCController controllerWithExtendedGamepad] (tvOS 13: a controller whose elements are set by the application),
 * makes +controllers and +current answer it, and posts GCControllerDidConnectNotification. Commands are read from
 * /var/tmp/twlpad/cmd (the directory must be writable by the application's user), one a line, the file removed
 * once done:
 *   press A|B|X|Y|L1|R1|L2|R2|MENU|OPTIONS|L3|R3 [SECONDS]   down, held (default 0.15 s), up
 *   hold BUTTON / release BUTTON
 *   dpad up|down|left|right [SECONDS]                       pressed for a moment (default 0.15 s)
 *   stick left|right X Y [SECONDS]                          a thumbstick at (X, Y), -1..1, then back to 0 (0: stays)
 *   wait SECONDS
 * What happened goes to stderr (with libtwlcrash: /var/tmp/twlcrash/NAME-PID.err).
 *
 * And a real one: the gamepads the kernel decodes (bsd/dev/hakios.gamepad.c, sysctl kern.hakios_gamepads -- there is no
 * IOHIDFamily for GameController to find them by) are read every 8 ms; the first one connected drives the controller,
 * which is announced when it comes and withdrawn when it goes. Its Start also calls the controller's paused handler.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef void *obj_t;
#define CMD_PATH "/var/tmp/twlpad/cmd"

static obj_t pad;               /* the GCController */
static obj_t gamepad;           /* its GCExtendedGamepad */
static int announced;           /* connected, as far as the application knows */

static obj_t
send0(obj_t o, const char *sel)
{
	return o ? ((obj_t (*)(obj_t, SEL))objc_msgSend)(o, sel_registerName(sel)) : NULL;
}

static obj_t
my_controllers(obj_t self, SEL sel)
{
	(void)self;
	(void)sel;
	return pad && announced ? ((obj_t (*)(obj_t, SEL, obj_t))objc_msgSend)((obj_t)objc_getClass("NSArray"), sel_registerName("arrayWithObject:"), pad)
	           : ((obj_t (*)(obj_t, SEL))objc_msgSend)((obj_t)objc_getClass("NSArray"), sel_registerName("array"));
}

static obj_t
my_current(obj_t self, SEL sel)
{
	(void)self;
	(void)sel;
	return announced ? pad : NULL;
}

/* the game's own controller code, watched (Minecraft's GameControllerHandler_iOS): each call written down, then made */
static IMP orig_button, orig_connect, orig_setup;

static void
my_processButton(obj_t self, SEL sel, int button, BOOL state)
{
	fprintf(stderr, "twlpad: the game: processButton:%d withState:%d\n", button, state);
	((void (*)(obj_t, SEL, int, BOOL))orig_button)(self, sel, button, state);
}

static void
my_onConnect(obj_t self, SEL sel, obj_t note)
{
	fprintf(stderr, "twlpad: the game: onConnectController:%p\n", note);
	((void (*)(obj_t, SEL, obj_t))orig_connect)(self, sel, note);
}

static void
my_setup(obj_t self, SEL sel, obj_t controller)
{
	fprintf(stderr, "twlpad: the game: setupController:%p\n", controller);
	((void (*)(obj_t, SEL, obj_t))orig_setup)(self, sel, controller);
}

static void
watch_game(void)
{
	Class c = objc_getClass("GameControllerHandler_iOS");
	Method m;

	if (c == NULL) {
		return;
	}
	if ((m = class_getInstanceMethod(c, sel_registerName("processButton:withState:"))) != NULL) {
		orig_button = method_setImplementation(m, (IMP)my_processButton);
	}
	if ((m = class_getInstanceMethod(c, sel_registerName("onConnectController:"))) != NULL) {
		orig_connect = method_setImplementation(m, (IMP)my_onConnect);
	}
	if ((m = class_getInstanceMethod(c, sel_registerName("setupController:"))) != NULL) {
		orig_setup = method_setImplementation(m, (IMP)my_setup);
	}
	fprintf(stderr, "twlpad: watching %s\n", class_getName(c));
}

/* ---- on the main queue ---- */

struct set {
	const char *element;    /* a GCExtendedGamepad property: buttonA, dpad, leftThumbstick ... */
	float x, y;
	int axes;               /* 2: a direction pad or stick (x, y); else a button (x) */
};

/*
 * A controller made by the application does not call the handlers hung on its elements when a value is set (setValue: only
 * stores it): they are called here, as a real controller's would be. A block's function is at offset 16 of the block.
 *   kind 1: (button, float value, BOOL pressed)   kind 2: (dpad, float x, float y)   kind 3: (gamepad, element)
 */
static void
call_block(obj_t block, int kind, obj_t sender, float a, float b, ...)
{
	if (block == NULL) {
		return;
	}
	void *invoke = *(void **)((char *)block + 16);
	if (kind == 1) {
		((void (*)(obj_t, obj_t, float, BOOL))invoke)(block, sender, a, b != 0);
	} else if (kind == 2) {
		((void (*)(obj_t, obj_t, float, float))invoke)(block, sender, a, b);
	} else {
		va_list ap;
		va_start(ap, b);
		obj_t element = va_arg(ap, obj_t);
		va_end(ap);
		((void (*)(obj_t, obj_t, obj_t))invoke)(block, sender, element);
	}
}

static void
button_handlers(obj_t button)
{
	if (button == NULL) {
		return;
	}
	float v = ((float (*)(obj_t, SEL))objc_msgSend)(button, sel_registerName("value"));
	BOOL said = ((BOOL (*)(obj_t, SEL))objc_msgSend)(button, sel_registerName("isPressed")), pressed = v > 0.5f;
	if (said != pressed) {
		fprintf(stderr, "twlpad: %s isPressed says %d at value %.2f: %d given\n", object_getClassName(button), said, (double)v, pressed);
	}
	call_block(send0(button, "valueChangedHandler"), 1, button, v, pressed);
	call_block(send0(button, "pressedChangedHandler"), 1, button, v, pressed);
}

static void
set_on_main(void *arg)
{
	struct set *s = arg;
	obj_t e = send0(gamepad, s->element);

	if (e == NULL) {
		fprintf(stderr, "twlpad: the gamepad has no %s\n", s->element);
		return;
	}
	if (s->axes == 2) {
		((void (*)(obj_t, SEL, float, float))objc_msgSend)(e, sel_registerName("setValueForXAxis:yAxis:"), s->x, s->y);
		/* (dpad, x, y), then each of its four buttons as a button */
		call_block(send0(e, "valueChangedHandler"), 2, e, s->x, s->y);
		static const char *const dirs[] = { "up", "down", "left", "right" };
		for (unsigned i = 0; i < 4; i++) {
			button_handlers(send0(e, dirs[i]));
		}
	} else {
		((void (*)(obj_t, SEL, float))objc_msgSend)(e, sel_registerName("setValue:"), s->x);
		button_handlers(e);
	}
	/* (gamepad, element) */
	call_block(send0(gamepad, "valueChangedHandler"), 3, gamepad, 0, 0, e);
}

static void
set(const char *element, float x, float y, int axes)
{
	struct set s = { element, x, y, axes };

	dispatch_sync_f(dispatch_get_main_queue(), &s, set_on_main);
}

static void
connect_on_main(void *arg)
{
	Class gc = objc_getClass("GCController");
	obj_t name;
	void *note;

	(void)arg;
	if (gc == NULL) {
		fprintf(stderr, "twlpad: no GCController class\n");
		return;
	}
	if (announced) {
		return;
	}
	if (pad == NULL) {
		pad = send0((obj_t)gc, "controllerWithExtendedGamepad");
		gamepad = send0(pad, "extendedGamepad");
		if (pad == NULL || gamepad == NULL) {
			fprintf(stderr, "twlpad: +controllerWithExtendedGamepad gave %p (gamepad %p)\n", pad, gamepad);
			pad = NULL;
			return;
		}
		((void (*)(obj_t, SEL))objc_msgSend)(pad, sel_registerName("retain"));
		((void (*)(obj_t, SEL, long))objc_msgSend)(pad, sel_registerName("setPlayerIndex:"), 0);
		method_setImplementation(class_getClassMethod(gc, sel_registerName("controllers")), (IMP)my_controllers);
		if (class_getClassMethod(gc, sel_registerName("current")) != NULL) {
			method_setImplementation(class_getClassMethod(gc, sel_registerName("current")), (IMP)my_current);
		}
	}
	announced = 1;
	note = dlsym(RTLD_DEFAULT, "GCControllerDidConnectNotification");
	name = note ? *(obj_t *)note : NULL;
	if (name != NULL) {
		obj_t center = send0((obj_t)objc_getClass("NSNotificationCenter"), "defaultCenter");
		((void (*)(obj_t, SEL, obj_t, obj_t))objc_msgSend)(center, sel_registerName("postNotificationName:object:"), name, pad);
	}
	fprintf(stderr, "twlpad: a virtual extended gamepad is connected (%p), commands from " CMD_PATH "\n", pad);
}

static void
disconnect_on_main(void *arg)
{
	void *note = dlsym(RTLD_DEFAULT, "GCControllerDidDisconnectNotification");

	(void)arg;
	if (!announced) {
		return;
	}
	announced = 0;
	if (note != NULL) {
		obj_t center = send0((obj_t)objc_getClass("NSNotificationCenter"), "defaultCenter");
		((void (*)(obj_t, SEL, obj_t, obj_t))objc_msgSend)(center, sel_registerName("postNotificationName:object:"), *(obj_t *)note, pad);
	}
	fprintf(stderr, "twlpad: the controller is disconnected\n");
}

/* ---- the real gamepads, as the kernel decodes them ---- */

struct kpad {                   /* bsd/dev/hakios.gamepad.c's struct hakios_gamepad */
	uint32_t seq, connected, buttons;
	int16_t lx, ly, rx, ry;
	uint16_t l2, r2;
	char name[32];
};
enum {
	K_A = 1u << 0, K_B = 1u << 1, K_X = 1u << 2, K_Y = 1u << 3, K_L1 = 1u << 4, K_R1 = 1u << 5, K_L2 = 1u << 6, K_R2 = 1u << 7,
	K_L3 = 1u << 8, K_R3 = 1u << 9, K_MENU = 1u << 10, K_OPTIONS = 1u << 11, K_HOME = 1u << 12, K_UP = 1u << 13, K_DOWN = 1u << 14,
	K_LEFT = 1u << 15, K_RIGHT = 1u << 16,
};

struct pad_change {
	struct kpad now, before;
};

static void
apply_on_main(void *arg)
{
	struct pad_change *c = arg;
	const struct kpad *n = &c->now, *b = &c->before;
	static const struct { uint32_t bit; const char *element; } buttons[] = {
		{ K_A, "buttonA" }, { K_B, "buttonB" }, { K_X, "buttonX" }, { K_Y, "buttonY" }, { K_L1, "leftShoulder" },
		{ K_R1, "rightShoulder" }, { K_L3, "leftThumbstickButton" }, { K_R3, "rightThumbstickButton" }, { K_MENU, "buttonMenu" },
		{ K_OPTIONS, "buttonOptions" },
	};

	if (!announced || gamepad == NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if ((n->buttons ^ b->buttons) & buttons[i].bit) {
			struct set s = { buttons[i].element, (n->buttons & buttons[i].bit) ? 1 : 0, 0, 1 };
			set_on_main(&s);
		}
	}
	if (n->l2 != b->l2) {
		struct set s = { "leftTrigger", n->l2 / 65535.0f, 0, 1 };
		set_on_main(&s);
	}
	if (n->r2 != b->r2) {
		struct set s = { "rightTrigger", n->r2 / 65535.0f, 0, 1 };
		set_on_main(&s);
	}
	if ((n->buttons ^ b->buttons) & (K_UP | K_DOWN | K_LEFT | K_RIGHT)) {
		struct set s = { "dpad", (float)(!!(n->buttons & K_RIGHT) - !!(n->buttons & K_LEFT)),
			         (float)(!!(n->buttons & K_UP) - !!(n->buttons & K_DOWN)), 2 };
		set_on_main(&s);
	}
	if (n->lx != b->lx || n->ly != b->ly) {
		struct set s = { "leftThumbstick", n->lx / 32767.0f, n->ly / 32767.0f, 2 };
		set_on_main(&s);
	}
	if (n->rx != b->rx || n->ry != b->ry) {
		struct set s = { "rightThumbstick", n->rx / 32767.0f, n->ry / 32767.0f, 2 };
		set_on_main(&s);
	}
	/* Start: a game's pause (GCController's controllerPausedHandler, which a real controller's Menu button calls) */
	if ((n->buttons & K_MENU) && !(b->buttons & K_MENU)) {
		obj_t paused = send0(pad, "controllerPausedHandler");
		if (paused != NULL) {
			((void (*)(obj_t, obj_t))(*(void **)((char *)paused + 16)))(paused, pad);
		}
	}
}

/* the application in front and active: only then is the pad its (reading it claims it from tvOS's keys, hakios.gamepad.c),
 * so that a system alert over it (inactive) can still be answered with the pad */
static volatile int app_active = 1;

static void
check_active_on_main(void *arg)
{
	obj_t app = send0((obj_t)objc_getClass("UIApplication"), "sharedApplication");

	(void)arg;
	if (app != NULL) {
		app_active = ((long (*)(obj_t, SEL))objc_msgSend)(app, sel_registerName("applicationState")) == 0;
	}
}

/* the first connected gamepad; 0 when there is none */
static int
read_pads(struct kpad *out)
{
	struct kpad pads[4];
	size_t size = sizeof(pads);

	if (sysctlbyname("kern.hakios_gamepads", pads, &size, NULL, 0) != 0 || size < sizeof(pads)) {
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		if (pads[i].connected) {
			*out = pads[i];
			return 1;
		}
	}
	return 0;
}

/* what the application hung on the controller: its handlers (a game either polls the values or is told through these) */
static void
status_on_main(void *arg)
{
	obj_t a = send0(gamepad, "buttonA");

	(void)arg;
	fprintf(stderr, "twlpad: status: controllers %p (count %ld); gamepad valueChangedHandler %p, buttonA value %.2f valueChangedHandler %p "
	    "pressedChangedHandler %p; controllerPausedHandler %p; gamepad (GCGamepad) %p, microGamepad %p, vendor %p\n",
	    send0((obj_t)objc_getClass("GCController"), "controllers"),
	    (long)((long (*)(obj_t, SEL))objc_msgSend)(send0((obj_t)objc_getClass("GCController"), "controllers"), sel_registerName("count")),
	    send0(gamepad, "valueChangedHandler"), (double)((float (*)(obj_t, SEL))objc_msgSend)(a, sel_registerName("value")),
	    send0(a, "valueChangedHandler"), send0(a, "pressedChangedHandler"), send0(pad, "controllerPausedHandler"),
	    class_respondsToSelector(object_getClass(pad), sel_registerName("gamepad")) ? send0(pad, "gamepad") : NULL,
	    send0(pad, "microGamepad"), send0(pad, "vendorName"));
}

static void
profile_status(const char *what, obj_t prof)
{
	obj_t a = prof && class_respondsToSelector(object_getClass(prof), sel_registerName("buttonA")) ? send0(prof, "buttonA") : NULL;
	fprintf(stderr, "twlpad:   %s: %p %s, valueChangedHandler %p; buttonA %p %s value handler %p pressed handler %p\n", what, prof,
	    prof ? object_getClassName(prof) : "-", prof && class_respondsToSelector(object_getClass(prof), sel_registerName("valueChangedHandler")) ?
	    send0(prof, "valueChangedHandler") : NULL, a, a ? object_getClassName(a) : "-", a ? send0(a, "valueChangedHandler") : NULL,
	    a ? send0(a, "pressedChangedHandler") : NULL);
}

static void
profiles_on_main(void *arg)
{
	(void)arg;
	profile_status("extendedGamepad", gamepad);
	profile_status("gamepad", class_respondsToSelector(object_getClass(pad), sel_registerName("gamepad")) ? send0(pad, "gamepad") : NULL);
	profile_status("microGamepad", send0(pad, "microGamepad"));
	{
		obj_t h = send0(send0(gamepad, "buttonA"), "valueChangedHandler");
		void *fn = h ? *(void **)((char *)h + 16) : NULL;
		Dl_info info = { 0 };
		dladdr(fn, &info);
		fprintf(stderr, "twlpad:   buttonA handler block %p, its function %p = %s + %#lx (%s); the program's slide %#lx\n", h, fn,
		    info.dli_fname ? info.dli_fname : "?", (unsigned long)((char *)fn - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?",
		    (unsigned long)_dyld_get_image_vmaddr_slide(0));
	}
	fprintf(stderr, "twlpad:   controller %p %s, attached %d\n", pad, object_getClassName(pad),
	    (int)((BOOL (*)(obj_t, SEL))objc_msgSend)(pad, sel_registerName("isAttachedToDevice")));
}

/* Minecraft's gamepad manager (data at 0x101654be8 of the executable): +0x28, a flag that has to be set for a button to count */
static uint8_t *
game_address(uint64_t vmaddr)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t n = name ? strlen(name) : 0;
		if (n > 17 && strcmp(name + n - 17, "/minecraftappletv") == 0) {
			return (uint8_t *)(uintptr_t)(vmaddr + (uint64_t)_dyld_get_image_vmaddr_slide(i));
		}
	}
	return NULL;
}

static void
app_on_main(void *arg)
{
	obj_t app = send0((obj_t)objc_getClass("UIApplication"), "sharedApplication");
	uint8_t *flag = game_address(0x101654be8 + 0x28);

	(void)arg;
	{
		obj_t scenes = send0(app, "connectedScenes");
		obj_t scene = send0(scenes, "anyObject");
		obj_t fbs = scene && class_respondsToSelector(object_getClass(scene), sel_registerName("_FBSScene")) ? send0(scene, "_FBSScene") : NULL;
		obj_t settings = send0(fbs, "settings");
		obj_t text = send0(settings, "description");
		char buf[1500] = "none";
		if (text != NULL) {
			CFStringGetCString((CFStringRef)text, buf, sizeof(buf), kCFStringEncodingUTF8);
		}
		for (char *c = buf; *c; c++) {
			if (*c == '\n') {
				*c = ' ';
			}
		}
		fprintf(stderr, "twlpad:   scene %p (%s), activation state %ld; settings: %s\n", scene, scene ? object_getClassName(scene) : "-",
		    scene ? ((long (*)(obj_t, SEL))objc_msgSend)(scene, sel_registerName("activationState")) : -9L, buf);
	}
	fprintf(stderr, "twlpad:   application state %ld (0 active, 1 inactive, 2 background); the game's gamepad flag %d (at %p)\n",
	    app ? ((long (*)(obj_t, SEL))objc_msgSend)(app, sel_registerName("applicationState")) : -1L, flag ? *flag : -1, flag);
}

/* the controller announced again: gone, then back (for a game whose engine was not listening yet when it first came) */
static void
reconnect_on_main(void *arg)
{
	obj_t center = send0((obj_t)objc_getClass("NSNotificationCenter"), "defaultCenter");
	void *gone = dlsym(RTLD_DEFAULT, "GCControllerDidDisconnectNotification"), *back = dlsym(RTLD_DEFAULT, "GCControllerDidConnectNotification");

	(void)arg;
	if (gone != NULL) {
		((void (*)(obj_t, SEL, obj_t, obj_t))objc_msgSend)(center, sel_registerName("postNotificationName:object:"), *(obj_t *)gone, pad);
	}
	if (back != NULL) {
		((void (*)(obj_t, SEL, obj_t, obj_t))objc_msgSend)(center, sel_registerName("postNotificationName:object:"), *(obj_t *)back, pad);
	}
	fprintf(stderr, "twlpad: disconnected and connected again\n");
}

/* ---- commands ---- */

static const char *
button(const char *name)
{
	static const struct { const char *name, *element; } map[] = {
		{ "A", "buttonA" }, { "B", "buttonB" }, { "X", "buttonX" }, { "Y", "buttonY" },
		{ "L1", "leftShoulder" }, { "R1", "rightShoulder" }, { "L2", "leftTrigger" }, { "R2", "rightTrigger" },
		{ "MENU", "buttonMenu" }, { "OPTIONS", "buttonOptions" }, { "L3", "leftThumbstickButton" }, { "R3", "rightThumbstickButton" },
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcasecmp(name, map[i].name) == 0) {
			return map[i].element;
		}
	}
	return NULL;
}

static void
command(char *line)
{
	char verb[16] = "", a[16] = "", b[16] = "", c[16] = "", d[16] = "";
	int n = sscanf(line, "%15s %15s %15s %15s %15s", verb, a, b, c, d);
	const char *e;

	if (n < 1) {
		return;
	}
	fprintf(stderr, "twlpad: %s", line);
	if ((strcmp(verb, "press") == 0 || strcmp(verb, "hold") == 0 || strcmp(verb, "release") == 0) && (e = button(a)) != NULL) {
		if (strcmp(verb, "release") != 0) {
			set(e, 1, 0, 1);
		}
		if (strcmp(verb, "press") == 0) {
			usleep((useconds_t)((n >= 3 ? atof(b) : 0.15) * 1e6));
		}
		if (strcmp(verb, "hold") != 0) {
			set(e, 0, 0, 1);
		}
	} else if (strcmp(verb, "dpad") == 0 && n >= 2) {
		float x = strcmp(a, "left") == 0 ? -1 : strcmp(a, "right") == 0 ? 1 : 0;
		float y = strcmp(a, "up") == 0 ? 1 : strcmp(a, "down") == 0 ? -1 : 0;
		set("dpad", x, y, 2);
		usleep((useconds_t)((n >= 3 ? atof(b) : 0.15) * 1e6));
		set("dpad", 0, 0, 2);
	} else if (strcmp(verb, "stick") == 0 && n >= 4) {
		const char *stick = strcmp(a, "right") == 0 ? "rightThumbstick" : "leftThumbstick";
		double hold = n >= 5 ? atof(d) : 0.15;
		set(stick, (float)atof(b), (float)atof(c), 2);
		if (hold > 0) {
			usleep((useconds_t)(hold * 1e6));
			set(stick, 0, 0, 2);
		}
	} else if (strcmp(verb, "reconnect") == 0) {
		dispatch_sync_f(dispatch_get_main_queue(), NULL, reconnect_on_main);
	} else if (strcmp(verb, "status") == 0) {
		dispatch_sync_f(dispatch_get_main_queue(), NULL, status_on_main);
		dispatch_sync_f(dispatch_get_main_queue(), NULL, profiles_on_main);
		dispatch_sync_f(dispatch_get_main_queue(), NULL, app_on_main);
	} else if (strcmp(verb, "wait") == 0 && n >= 2) {
		usleep((useconds_t)(atof(a) * 1e6));
	} else {
		fprintf(stderr, "twlpad: not understood\n");
	}
}

static void *
commands(void *arg)
{
	struct kpad last = { 0 }, now;
	int real = 0, by_real = 0;
	unsigned tick = 0;

	(void)arg;
	sleep(3);       /* let the application set up its GameController observers first */
	for (;; tick++) {
		if (tick % 30 == 0) {
			dispatch_async_f(dispatch_get_main_queue(), NULL, check_active_on_main);
		}
		if (!app_active) {
			usleep(8000);
			continue;
		}
		real = read_pads(&now);
		if (real && !announced) {
			fprintf(stderr, "twlpad: gamepad \"%s\" from the kernel\n", now.name);
			dispatch_sync_f(dispatch_get_main_queue(), NULL, connect_on_main);
			by_real = 1;
			memset(&last, 0, sizeof(last));
		} else if (!real && announced && by_real) {
			dispatch_sync_f(dispatch_get_main_queue(), NULL, disconnect_on_main);
			by_real = 0;
		}
		if (real && now.seq != last.seq) {
			struct pad_change c = { now, last };
			dispatch_sync_f(dispatch_get_main_queue(), &c, apply_on_main);
			last = now;
		}
		if (tick % 6 == 0) {
			FILE *f = fopen(CMD_PATH, "r");
			if (f != NULL) {
				char line[128];
				unlink(CMD_PATH);
				if (!announced) {
					dispatch_sync_f(dispatch_get_main_queue(), NULL, connect_on_main);
				}
				while (fgets(line, sizeof(line), f) != NULL) {
					if (pad != NULL) {
						command(line);
					}
				}
				fclose(f);
			}
		}
		usleep(8000);
	}
	return NULL;
}

__attribute__((constructor)) static void
twlpad_init(void)
{
	pthread_t t;

	dlopen("/System/Library/Frameworks/GameController.framework/GameController", RTLD_NOW);
	watch_game();
	if (pthread_create(&t, NULL, commands, NULL) == 0) {
		pthread_detach(t);
	}
}
