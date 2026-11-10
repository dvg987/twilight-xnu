/*
 * twlquiet (loaded by twlload: /var/mobile/twl/load.conf "mediaserverd /var/mobile/twl/libtwlquiet.dylib"): PowerLog's
 * client calls answer at once and log nothing. In mediaserverd during 4K playback a quarter of the running time was
 * PowerLog's client queue (objc_sync_exit, ulock wakes) and a third more the dispatch worker threads it kept starting;
 * the box has no PowerLog daemon worth feeding. Each entry point's first instructions become "mov w0, #0; ret"
 * (PLShouldLogRegisteredEvent: NO; the others return nothing).
 */
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void
quiet(const char *name)
{
	uint32_t *f = (uint32_t *)dlsym(RTLD_DEFAULT, name);

	if (f == NULL) {
		fprintf(stderr, "twlquiet: no %s\n", name);
		return;
	}
	/* (an arm64e-style pointer would carry a signature; this system's do not) */
	vm_address_t page = (vm_address_t)f & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t size = ((vm_address_t)f + 8 > page + vm_page_size) ? 2 * vm_page_size : vm_page_size;

	if (vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		fprintf(stderr, "twlquiet: %s: text not writable\n", name);
		return;
	}
	f[0] = 0x52800000u;     /* mov w0, #0 */
	f[1] = 0xd65f03c0u;     /* ret */
	vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(f, 8);
	fprintf(stderr, "twlquiet: %s off\n", name);
}

__attribute__((constructor)) static void
start(void)
{
	/* PowerLog is loaded by then in a program that uses it; dlopen it to be sure (NOLOAD: never bring it in) */
	dlopen("/System/Library/PrivateFrameworks/PowerLog.framework/PowerLog", RTLD_NOW | RTLD_NOLOAD);
	quiet("PLShouldLogRegisteredEvent");
	quiet("PLLogRegisteredEvent");
	quiet("PLLogTimeSensitiveRegisteredEvent");
}
