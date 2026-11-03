/*
 * BlueTool for a controller that is booted by the kernel: bluetoothd asks com.apple.BlueTool to run its "boot" script
 * (download the Broadcom chip's firmware, set its baud rate, address and sleep mode) and waits for result 0 before it
 * opens the UART itself. Here the controller is the Amlogic W2's, which the kernel has started already
 * (hakios.w2bt.c, w2bt=3), so every request is answered: done, by a Broadcom chip (manufacturer 15).
 * No XPC headers in the SDK: what is used of libxpc is declared here.
 */
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern char *xpc_copy_description(xpc_object_t object);
extern void xpc_release(xpc_object_t object);

#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		fprintf(stderr, "bluetool: com.apple.BlueTool cannot be served\n");
		return 1;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			xpc_object_t reply;
			char *what;

			if (xpc_get_type(message) != &_xpc_type_dictionary) {
				return;
			}
			what = xpc_copy_description(message);
			fprintf(stderr, "bluetool: asked %s: done\n", what != NULL ? what : "?");
			free(what);
			reply = xpc_dictionary_create_reply(message);
			if (reply != NULL) {
				xpc_dictionary_set_int64(reply, "result", 0);
				xpc_dictionary_set_int64(reply, "manufacturer", 15);
				xpc_connection_send_message(peer, reply);
				xpc_release(reply);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	fprintf(stderr, "bluetool: serving com.apple.BlueTool (the controller is booted by the kernel)\n");
	dispatch_main();
}
