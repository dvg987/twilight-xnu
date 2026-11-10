/*
 * libtwlcrash.dylib: a crash logger for an application tvOS starts (there is no crash reporter on the box).
 * Inserted by the kernel (kern.exec_inject, etc/inject.conf), it sends the program's standard error to
 * /var/tmp/twlcrash/NAME-PID.err and, when the program dies of a signal or an uncaught Objective-C exception,
 * writes there what killed it, the faulting address, a backtrace and the loaded images with their slides
 * (to symbolize the backtrace on the computer).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

typedef void *obj_t;

#define SEND0(type, obj, sel) ((type (*)(obj_t, SEL))objc_msgSend)((obj), sel_registerName(sel))

static int log_fd = 2;
static char stack[64 * 1024];

static void
put(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		(void)write(log_fd, line, (size_t)n);
	}
}

static void
images(void)
{
	put("images:\n");
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const struct mach_header *mh = _dyld_get_image_header(i);
		const char *name = _dyld_get_image_name(i);

		/* the shared cache's images are many and one slide: the program's own and the rest outside it */
		if (name != NULL && (strstr(name, "/System/") == NULL && strstr(name, "/usr/lib/") == NULL)) {
			put("  %p slide %#lx %s\n", (const void *)mh, (unsigned long)_dyld_get_image_vmaddr_slide(i), name);
		}
	}
}

static void
backtrace_here(void)
{
	void *frames[128];
	int n = backtrace(frames, 128);

	put("backtrace (%d frames):\n", n);
	backtrace_symbols_fd(frames, n, log_fd);
}

static void
on_signal(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	put("\n*** signal %d (%s), code %d, address %p\n", sig, strsignal(sig), info->si_code, info->si_addr);
	if (uc != NULL && uc->uc_mcontext != NULL) {
		put("pc %#llx lr %#llx sp %#llx fp %#llx far %#llx esr %#x\n", uc->uc_mcontext->__ss.__pc,
		    uc->uc_mcontext->__ss.__lr, uc->uc_mcontext->__ss.__sp, uc->uc_mcontext->__ss.__fp,
		    uc->uc_mcontext->__es.__far, uc->uc_mcontext->__es.__esr);
	}
	if (uc != NULL && uc->uc_mcontext != NULL) {
		/* the frame-pointer chain of the faulting thread: backtrace() here would walk the signal stack */
		uint64_t fp = uc->uc_mcontext->__ss.__fp;

		put("frames: pc %#llx lr %#llx\n", uc->uc_mcontext->__ss.__pc, uc->uc_mcontext->__ss.__lr);
		for (int i = 0; i < 64 && fp != 0 && (fp & 7) == 0; i++) {
			uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1];

			put("  #%d %#llx\n", i, ret & 0x0000000fffffffffULL);
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	images();
	fsync(log_fd);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
on_exception(obj_t exception)
{
	obj_t name = SEND0(obj_t, exception, "name"), reason = SEND0(obj_t, exception, "reason");
	obj_t symbols = SEND0(obj_t, exception, "callStackSymbols");
	char buf[1024];

	put("\n*** uncaught exception\n");
	if (name != NULL && CFStringGetCString((CFStringRef)name, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		put("name: %s\n", buf);
	}
	if (reason != NULL && CFStringGetCString((CFStringRef)reason, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		put("reason: %s\n", buf);
	}
	if (symbols != NULL) {
		CFIndex n = CFArrayGetCount((CFArrayRef)symbols);

		for (CFIndex i = 0; i < n; i++) {
			CFStringRef s = CFArrayGetValueAtIndex((CFArrayRef)symbols, i);

			if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
				put("  %s\n", buf);
			}
		}
	}
	images();
	fsync(log_fd);
}

/* how a program that is not killed by a signal of its own ends: exit() (who called it) or a SIGTERM from outside;
 * one that leaves no line at all was SIGKILLed (FrontBoard, the memory watchdog) */
static void
on_exit_here(void)
{
	put("\n*** exit() called\n");
	backtrace_here();
	fsync(log_fd);
}

static void
on_term(int sig)
{
	put("\n*** signal %d (%s) from outside\n", sig, strsignal(sig));
	backtrace_here();
	fsync(log_fd);
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void
twl_crash_init(void)
{
	char path[1024], file[1100];
	uint32_t size = sizeof(path);
	const char *base;
	stack_t ss = { .ss_sp = stack, .ss_size = sizeof(stack), .ss_flags = 0 };
	struct sigaction sa;
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP, SIGSYS };
	void (*set_handler)(void (*)(obj_t));

	if (_NSGetExecutablePath(path, &size) != 0) {
		return;
	}
	base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	mkdir("/var/tmp/twlcrash", 01777);
	snprintf(file, sizeof(file), "/var/tmp/twlcrash/%s-%d.err", base, getpid());
	int fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		dup2(fd, 2);
		close(fd);
		log_fd = 2;
	}
	setvbuf(stderr, NULL, _IONBF, 0);
	put("twlcrash: %s, pid %d, uid %d\n", path, getpid(), getuid());
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_signal;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		sigaction(sigs[i], &sa, NULL);
	}
	signal(SIGTERM, on_term);
	atexit(on_exit_here);
	dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
	set_handler = (void (*)(void (*)(obj_t)))dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
	if (set_handler != NULL) {
		set_handler(on_exception);
	}
}
