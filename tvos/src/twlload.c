/*
 * twlload: injected into a few daemons; loads what /var/mobile/twl/load.conf lists for this program ("PROGRAM PATH" lines), so that
 * diagnostic libraries can come and go without rebuilding the image.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void
load(void)
{
	FILE *f = fopen("/var/mobile/twl/load.conf", "r");
	if (f == NULL) return;
	const char *me = getprogname();
	char prog[128], path[512];
	while (fscanf(f, "%127s %511s", prog, path) == 2)
		if (prog[0] != '#' && strcmp(prog, me) == 0 && dlopen(path, RTLD_NOW) == NULL)
			fprintf(stderr, "twlload: %s: %s\n", path, dlerror());
	fclose(f);
}
