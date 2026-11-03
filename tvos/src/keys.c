/*
 * keys FILE: replay key presses into /dev/hakios-touch, for backboardd's twilight keys (tvos/src/twlinput.inc).
 * Lines: "wait SECONDS", or a key name (up down left right select menu playpause home), or "PAGE USAGE" in hex.
 * Static, raw system calls (HakiOS's libmini): it runs whatever dyld thinks of it.
 */
#include "mini.h"

struct hakios_touch { unsigned long long time; unsigned type, finger, x, y; };

static const struct { const char *name; unsigned code; } names[] = {
	{ "up", 0x07 << 16 | 0x52 }, { "down", 0x07 << 16 | 0x51 }, { "left", 0x07 << 16 | 0x50 }, { "right", 0x07 << 16 | 0x4f },
	{ "select", 0x07 << 16 | 0x28 }, { "menu", 0x07 << 16 | 0x29 }, { "playpause", 0x0c << 16 | 0xcd },
	{ "home", 0x0c << 16 | 0x223 }, { "rup", 0x0c << 16 | 0x42 }, { "rdown", 0x0c << 16 | 0x43 },
	{ "rleft", 0x0c << 16 | 0x44 }, { "rright", 0x0c << 16 | 0x45 }, { "rselect", 0x0c << 16 | 0x41 },
	{ "rmenu", 0x0c << 16 | 0x40 },
};

static unsigned
hex(const char *s, const char **end)
{
	unsigned v = 0;

	for (; *s; s++) {
		unsigned d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : 99;
		if (d > 15) {
			break;
		}
		v = v * 16 + d;
	}
	*end = s;
	return v;
}

static void
press(int dev, unsigned code)
{
	struct hakios_touch t = { 0, 4, code, 0, 0 };

	write(dev, &t, sizeof(t));
	usleep(80000);
	t.type = 5;
	write(dev, &t, sizeof(t));
	printf("keys: %#x\n", code);
}

int
main(int argc, char **argv, char **envp)
{
	static char buf[8192];
	int dev, fd, n;
	char *line;

	(void)envp;
	if (argc < 2 || (fd = open(argv[1], O_RDONLY, 0)) < 0) {
		printf("keys: no list\n");
		return 1;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	dev = open("/dev/hakios-touch", O_RDWR, 0);
	if (dev < 0) {
		printf("keys: /dev/hakios-touch: %s\n", strerror(errno));
		return 1;
	}
	for (line = buf; *line;) {
		char *next = strchr(line, '\n');
		const char *end;

		if (next) {
			*next++ = 0;
		} else {
			next = line + strlen(line);
		}
		if (strncmp(line, "wait ", 5) == 0) {
			unsigned s = 0;
			for (const char *c = line + 5; *c >= '0' && *c <= '9'; c++) s = s * 10 + (unsigned)(*c - '0');
			usleep(s * 1000000);
		} else if (line[0] && line[0] != '#') {
			int found = 0;
			for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
				if (strcmp(line, names[i].name) == 0) {
					press(dev, names[i].code);
					found = 1;
				}
			}
			if (!found) {
				unsigned page = hex(line, &end), usage = hex(end + (*end == ' '), &end);
				if (page && usage) press(dev, page << 16 | usage);
			}
			usleep(1500000);
		}
		line = next;
	}
	return 0;
}
