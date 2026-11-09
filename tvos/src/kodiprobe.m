/* Drive the real player's local JSON-RPC API; network loss must not change test timing. */
#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>

static unsigned sequence;
static id request(NSString *method, NSDictionary *params)
{
    unsigned identifier = ++sequence;
    NSMutableDictionary *request = [@{@"jsonrpc":@"2.0", @"id":@(identifier), @"method":method} mutableCopy];
    if (params) request[@"params"] = params;
    NSData *data = [NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = { .sin_len=sizeof(address), .sin_family=AF_INET,
        .sin_port=htons(9090), .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    if (fd < 0 || connect(fd, (void *)&address, sizeof(address))) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "kodiprobe: %s: connect failed\n", method.UTF8String);
        return nil;
    }
    const uint8_t *bytes = data.bytes;
    size_t left = data.length;
    while (left) {
        ssize_t n = send(fd, bytes, left, 0);
        if (n <= 0) { close(fd); return nil; }
        bytes += n; left -= (size_t)n;
    }
    NSMutableData *received = [NSMutableData data];
    char buffer[4096];
    int depth = 0;
    BOOL quoted = NO, escaped = NO;
    size_t parsed = 0, start = 0;
    while (received.length < 65536) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        [received appendBytes:buffer length:(NSUInteger)n];
        const uint8_t *input = received.bytes;
        while (parsed < received.length) {
            uint8_t c = input[parsed++];
            if (quoted) {
                if (escaped) escaped = NO;
                else if (c == '\\') escaped = YES;
                else if (c == '"') quoted = NO;
                continue;
            }
            if (c == '"') quoted = YES;
            else if (c == '{') { if (!depth) start = parsed - 1; depth++; }
            else if (c == '}' && --depth == 0) {
                NSData *message = [received subdataWithRange:NSMakeRange(start, parsed-start)];
                NSDictionary *reply = [NSJSONSerialization JSONObjectWithData:message options:0 error:nil];
                if ([reply[@"id"] unsignedIntValue] == identifier) {
                    close(fd);
                    fprintf(stderr, "kodiprobe: %s: %s\n", method.UTF8String,
                        [[NSString alloc] initWithData:message encoding:NSUTF8StringEncoding].UTF8String);
                    return reply[@"error"] ? nil : reply[@"result"];
                }
            }
        }
    }
    close(fd);
    fprintf(stderr, "kodiprobe: %s: no reply\n", method.UTF8String);
    return nil;
}
static NSDictionary *properties(void)
{
    return request(@"Player.GetProperties", @{@"playerid":@1,
        @"properties":@[@"time", @"speed", @"percentage", @"currentvideostream"]});
}
static BOOL open_player(NSString *path)
{
    if (!request(@"Player.Open", @{@"item":@{@"file":path}})) return NO;
    for (unsigned i=0; i<40; i++) {
        NSDictionary *state = properties();
        if ([state[@"speed"] intValue] == 1 && [state[@"currentvideostream"][@"width"] intValue] > 0) return YES;
        usleep(100000);
    }
    return NO;
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 2) { fprintf(stderr, "usage: kodiprobe LOCAL_MOVIE\n"); return 2; }
        setbuf(stderr, NULL);
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        BOOL ready = NO;
        for (unsigned i=0; i<60; i++) {
            if (request(@"JSONRPC.Ping", nil)) { ready = YES; break; }
            sleep(1);
        }
        if (!ready) return 3;
        for (unsigned iteration=0; iteration<2; iteration++) {
            if (!open_player(path)) return 3;
            if (!request(@"Player.PlayPause", @{@"playerid":@1, @"play":@NO})) return 4;
            NSDictionary *seek = request(@"Player.Seek", @{@"playerid":@1, @"value":@{@"percentage":@50.0}});
            if (!seek) return 5; /* Seeking is asynchronous; the reply can describe the old clock. */
            if (!request(@"Player.PlayPause", @{@"playerid":@1, @"play":@YES})) return 6;
            BOOL sought = NO;
            for (unsigned i=0; i<30; i++) {
                NSDictionary *state = properties();
                double percentage = [state[@"percentage"] doubleValue];
                if (state && percentage >= 40 && percentage <= 70) { sought = YES; break; }
                usleep(100000);
            }
            if (!sought) return 5;
            if (!request(@"Player.Stop", @{@"playerid":@1})) return 7;
            usleep(500000);
        }
        /* Leave a decoded picture paused for checking the real display output. */
        if (!open_player(path)) return 8;
        usleep(400000);
        if (!request(@"Player.PlayPause", @{@"playerid":@1, @"play":@NO})) return 9;
        NSDictionary *state = properties();
        if (!state || [state[@"speed"] intValue] != 0) return 10;
        fprintf(stderr, "kodiprobe: reopen/pause/seek/resume/stop: PASS; paused for display check\n");
        return 0;
    }
}
