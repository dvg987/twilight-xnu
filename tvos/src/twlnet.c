/*
 * TCP throughput, without ssh's ciphers on the box's small cores (tools/twl-net.py is the other end):
 *   twlnet sink PORT            accept one connection, read, report Mbit/s each second and at the end
 *   twlnet source HOST PORT S   connect and write for S seconds, the same reports
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int
run(int fd, int sending, double seconds)
{
	static char buf[256 * 1024];
	double start = now(), mark = start;
	long long total = 0, since = 0;
	for (;;) {
		ssize_t n = sending ? write(fd, buf, sizeof(buf)) : read(fd, buf, sizeof(buf));
		if (n <= 0) break;
		total += n;
		since += n;
		double t = now();
		if (t - mark >= 1) {
			printf("%.1f Mbit/s\n", since * 8 / (t - mark) / 1e6);
			fflush(stdout);
			mark = t;
			since = 0;
		}
		if (sending && t - start >= seconds) break;
	}
	double t = now() - start;
	printf("total %lld bytes in %.1f s: %.1f Mbit/s\n", total, t, t > 0 ? total * 8 / t / 1e6 : 0);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "sink")) {
		int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
		if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) { perror("listen"); return 1; }
		int fd = accept(s, NULL, NULL);
		if (fd < 0) { perror("accept"); return 1; }
		return run(fd, 0, 0);
	}
	if (argc >= 5 && !strcmp(argv[1], "source")) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(atoi(argv[3])) };
		inet_pton(AF_INET, argv[2], &a.sin_addr);
		if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 1; }
		return run(fd, 1, atof(argv[4]));
	}
	fprintf(stderr, "usage: twlnet sink PORT | twlnet source HOST PORT SECONDS\n");
	return 1;
}
