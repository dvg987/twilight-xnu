/*
 * libtwlcloud.dylib: an iCloud account as far as an application's own CloudKit calls go, for a game that will not
 * start without one (Minecraft's Apple TV edition) on a box that has none. Injected into the program.
 *
 *   -[CKContainer accountStatusWithCompletionHandler:]       available
 *   -[CKContainer fetchUserRecordIDWithCompletionHandler:]   a record ID of its own ("_twl_local_user")
 *   -[CKDatabase addOperation:]                              a CKQueryOperation finds nothing; a CKModifyRecordsOperation
 *                                                            saves and deletes everything it was given -- and nothing
 *                                                            is stored anywhere (a fresh, empty account each launch)
 * Every call is written to stderr (with libtwlcrash: /var/tmp/twlcrash/NAME-PID.err).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

typedef void *obj_t;

static obj_t
send0(obj_t o, const char *sel)
{
	if (o == NULL || !class_respondsToSelector(object_getClass(o), sel_registerName(sel))) {
		return NULL;
	}
	return ((obj_t (*)(obj_t, SEL))objc_msgSend)(o, sel_registerName(sel));
}

static void *
block_fn(obj_t block)
{
	return block ? *(void **)((char *)block + 16) : NULL;
}

static obj_t
retain(obj_t o)
{
	return o ? ((obj_t (*)(obj_t, SEL))objc_msgSend)(o, sel_registerName("retain")) : NULL;
}

static obj_t
copy_block(obj_t b)
{
	return b ? ((obj_t (*)(obj_t, SEL))objc_msgSend)(b, sel_registerName("copy")) : NULL;
}

/* ---- the container ---- */

static void
account_status_later(void *arg)
{
	obj_t handler = arg;
	((void (*)(obj_t, long, obj_t))block_fn(handler))(handler, 1 /* CKAccountStatusAvailable */, NULL);
}

static void
my_account_status(obj_t self, SEL sel, obj_t handler)
{
	(void)self;
	(void)sel;
	fprintf(stderr, "twlcloud: accountStatus: available\n");
	if (handler != NULL) {
		dispatch_async_f(dispatch_get_global_queue(0, 0), copy_block(handler), account_status_later);
	}
}

static obj_t user_id;

static void
user_id_later(void *arg)
{
	obj_t handler = arg;
	((void (*)(obj_t, obj_t, obj_t))block_fn(handler))(handler, user_id, NULL);
}

static void
my_fetch_user_id(obj_t self, SEL sel, obj_t handler)
{
	(void)self;
	(void)sel;
	if (user_id == NULL) {
		obj_t name = (obj_t)CFStringCreateWithCString(NULL, "_twl_local_user", kCFStringEncodingUTF8);
		user_id = ((obj_t (*)(obj_t, SEL, obj_t))objc_msgSend)(send0((obj_t)objc_getClass("CKRecordID"), "alloc"),
		    sel_registerName("initWithRecordName:"), name);
	}
	fprintf(stderr, "twlcloud: fetchUserRecordID: %p\n", user_id);
	if (handler != NULL) {
		dispatch_async_f(dispatch_get_global_queue(0, 0), copy_block(handler), user_id_later);
	}
}

/* ---- the database's operations ---- */

static long
count_of(obj_t array)
{
	return array ? ((long (*)(obj_t, SEL))objc_msgSend)(array, sel_registerName("count")) : 0;
}

static obj_t
at(obj_t array, long i)
{
	return ((obj_t (*)(obj_t, SEL, long))objc_msgSend)(array, sel_registerName("objectAtIndex:"), i);
}

static void
finish_operation(obj_t op)
{
	obj_t done = send0(op, "completionBlock");          /* NSOperation's own */
	if (done != NULL) {
		((void (*)(obj_t))block_fn(done))(done);
	}
}

static void
run_operation(void *arg)
{
	obj_t op = arg;
	Class query = objc_getClass("CKQueryOperation"), modify = objc_getClass("CKModifyRecordsOperation");

	if (query && ((BOOL (*)(obj_t, SEL, Class))objc_msgSend)(op, sel_registerName("isKindOfClass:"), query)) {
		obj_t q = send0(op, "query");
		obj_t type = send0(q, "recordType");
		char t[100] = "?";
		if (type) {
			CFStringGetCString((CFStringRef)type, t, sizeof(t), kCFStringEncodingUTF8);
		}
		fprintf(stderr, "twlcloud: a query for %s records: none\n", t);
		obj_t qdone = send0(op, "queryCompletionBlock");
		if (qdone != NULL) {
			((void (*)(obj_t, obj_t, obj_t))block_fn(qdone))(qdone, NULL, NULL);
		}
		obj_t rdone = send0(op, "queryResultBlock");    /* (tvOS 15's: a Result) -- not called: the app (2017) has the one above */
		(void)rdone;
	} else if (modify && ((BOOL (*)(obj_t, SEL, Class))objc_msgSend)(op, sel_registerName("isKindOfClass:"), modify)) {
		obj_t save = send0(op, "recordsToSave"), del = send0(op, "recordIDsToDelete");
		fprintf(stderr, "twlcloud: modify: %ld saved, %ld deleted (kept nowhere)\n", count_of(save), count_of(del));
		obj_t per = send0(op, "perRecordCompletionBlock");
		for (long i = 0; per != NULL && i < count_of(save); i++) {
			((void (*)(obj_t, obj_t, obj_t))block_fn(per))(per, at(save, i), NULL);
		}
		obj_t mdone = send0(op, "modifyRecordsCompletionBlock");
		if (mdone != NULL) {
			((void (*)(obj_t, obj_t, obj_t, obj_t))block_fn(mdone))(mdone, save, del, NULL);
		}
	} else {
		fprintf(stderr, "twlcloud: an operation of class %s: finished, nothing done\n", object_getClassName(op));
	}
	finish_operation(op);
}

static void
my_add_operation(obj_t self, SEL sel, obj_t op)
{
	(void)self;
	(void)sel;
	if (op != NULL) {
		dispatch_async_f(dispatch_get_global_queue(0, 0), retain(op), run_operation);
	}
}

static void
replace(const char *cls, const char *sel, IMP imp)
{
	Class c = objc_getClass(cls);
	Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
	if (m != NULL) {
		method_setImplementation(m, imp);
	} else {
		fprintf(stderr, "twlcloud: no -[%s %s]\n", cls, sel);
	}
}

__attribute__((constructor)) static void
twlcloud_init(void)
{
	if (dlopen("/System/Library/Frameworks/CloudKit.framework/CloudKit", RTLD_NOW) == NULL) {
		return;
	}
	replace("CKContainer", "accountStatusWithCompletionHandler:", (IMP)my_account_status);
	replace("CKContainer", "fetchUserRecordIDWithCompletionHandler:", (IMP)my_fetch_user_id);
	replace("CKDatabase", "addOperation:", (IMP)my_add_operation);
	fprintf(stderr, "twlcloud: CloudKit answers for itself: an account, an empty private database\n");
}
