/*
 * metaltest: the GPU's Metal plug-in checked on its own, before the window server is given it: loads
 * /System/Library/Extensions/NativeMaliMetal.bundle and runs its self-test (nmm_selftest: compute, a draw, an
 * IOSurface import, and QuartzCore's default.metallib translated function by function, through metal2vulkan and
 * PanVK to the kernel's MaliService). Its findings go to the console; the exit status is the self-test's code.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

#define PLUGIN "/System/Library/Extensions/NativeMaliMetal.bundle/NativeMaliMetal"

int
main(int argc, char **argv)
{
	int quartzcore = argc > 1 ? argv[1][0] - '0' : 1;
	void *plugin = dlopen(PLUGIN, RTLD_NOW);
	int (*selftest)(int);
	int code;

	if (plugin == NULL) {
		fprintf(stderr, "metaltest: %s\n", dlerror());
		return 100;
	}
	selftest = (int (*)(int))dlsym(plugin, "nmm_selftest");
	if (selftest == NULL) {
		fprintf(stderr, "metaltest: no nmm_selftest in the plug-in\n");
		return 101;
	}
	fprintf(stderr, "metaltest: self-test (QuartzCore's library: %s)\n", quartzcore ? "yes" : "no");
	code = selftest(quartzcore);
	fprintf(stderr, "metaltest: self-test returned %#x\n", code);
	return code;
}
