/*
 * Opt-in CoreVideo texture import for the replacement EAGL stack.
 * Apple's texture cache accesses its private GPU sharegroup. Our EAGL context instead
 * uses Mesa; upload immutable NV12 planes through GLES and retain each source until
 * the texture is released. This path copies pixels; decoding still runs on VDEC.
 * Deliberately limited to the source-texture operations used by Kodi's SDR renderer.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/hakios.vdec.h>
#include <OpenGLES/ES3/gl.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#ifndef GL_LUMINANCE
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190a
#endif

/* tvOS 16 arm64 CF runtime ABI; apple-oss-distributions/CF, CFRuntime.h. */
struct runtime_base { uintptr_t isa; uint8_t info[4]; uint32_t rc; };
struct runtime_class {
    CFIndex version;
    const char *name;
    void (*init)(CFTypeRef);
    void *copy;
    void (*finalize)(CFTypeRef);
    void *equal, *hash, *format, *description, *reclaim, *refcount;
    uintptr_t alignment;
};
/*
 * The video plane: a picture the hardware decoder tagged (TWL_VPLANE_TOKEN_KEY on its IOSurface) is shown by the
 * VPU under the OSD, at the output's resolution, and its textures here are a single texel of Y 60 Cb 255 Cr 255:
 * Kodi's shader turns that into the key colour (TWL_VPLANE_KEY_RGB), which the OSD makes transparent. Kodi still
 * places, scales and overlays the picture; the plane finds where from the key. Without the plane, ordinary textures.
 */
static int vplane = -2;
/* touch /var/tmp/twlvdec-debug: what the plane was asked for, in /var/tmp/twlcvgl.log */
static void debug(const char *format, ...)
{
    static unsigned lines;
    if (lines >= 400 || access("/var/tmp/twlvdec-debug", F_OK)) return;
    FILE *f = fopen("/var/tmp/twlcvgl.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fclose(f);
    lines++;
}
static CVPixelBufferRef keyed;          /* the last picture given the key */
/*
 * The plane is told at present time, not here: Kodi makes the textures of pictures before it shows them (and of some it
 * never shows), and told here the plane took them early, two in a vsync, while others were decoded over first (24p
 * judder). A keyed texture's GL name keeps its picture's token; the one bound last for drawing before
 * -[EAGLContext presentRenderbuffer:] is the frame's picture, and only then goes to the plane.
 */
#define KEYED_TEXTURES 32
static struct { GLuint name; struct twl_vplane_token token; } keyed_names[KEYED_TEXTURES];
static unsigned keyed_next;
static struct twl_vplane_token drawn;
static bool have_drawn;
static pthread_mutex_t keyed_lock = PTHREAD_MUTEX_INITIALIZER;
static bool plane_token(CVPixelBufferRef source, size_t plane, struct twl_vplane_token *token)
{
    if (vplane == -2) {
        vplane = getenv("TWL_NO_VPLANE") ? -1 : open("/dev/twl-vplane", O_RDWR);
        fprintf(stderr, "twlcvgl: video plane %s\n", vplane >= 0 ? "available" : "not used");
        debug("video plane: %s\n", vplane >= 0 ? "open" : strerror(errno));
    }
    if (vplane < 0) return false;
    if (plane) return source == keyed;
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(source);
    CFTypeRef value = surface ? IOSurfaceCopyValue(surface, CFSTR(TWL_VPLANE_TOKEN_KEY)) : NULL;
    bool ok = value && CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength(value) == sizeof(*token);
    if (ok) {
        memcpy(token, CFDataGetBytePtr(value), sizeof(*token));
        ok = token->magic == TWL_VPLANE_MAGIC;
    } else {
        debug("picture without a token (surface %p, value %p)\n", (void *)surface, value);
    }
    if (value) CFRelease(value);
    keyed = ok ? source : NULL;
    return ok;
}
static void keyed_add(GLuint name, const struct twl_vplane_token *token)
{
    pthread_mutex_lock(&keyed_lock);
    keyed_names[keyed_next % KEYED_TEXTURES].name = name;
    keyed_names[keyed_next % KEYED_TEXTURES].token = *token;
    keyed_next++;
    pthread_mutex_unlock(&keyed_lock);
}
static void keyed_remove(GLuint name)
{
    pthread_mutex_lock(&keyed_lock);
    for (unsigned i = 0; i < KEYED_TEXTURES; i++) if (keyed_names[i].name == name) keyed_names[i].name = 0;
    pthread_mutex_unlock(&keyed_lock);
}
/* the keyed picture bound on each texture unit (Kodi binds a picture's texture to set it up, before it draws it) */
#define UNITS 8
static GLenum active_unit;
static struct { bool keyed; struct twl_vplane_token token; } unit_bound[UNITS];
static void my_active_texture(GLenum unit)
{
    glActiveTexture(unit);
    active_unit = unit - GL_TEXTURE0;
}
static void my_bind_texture(GLenum target, GLuint name)
{
    glBindTexture(target, name);
    if (target != GL_TEXTURE_2D || active_unit >= UNITS) return;
    pthread_mutex_lock(&keyed_lock);
    unit_bound[active_unit].keyed = false;
    for (unsigned i = 0; name && i < KEYED_TEXTURES; i++) {
        if (keyed_names[i].name == name) {
            unit_bound[active_unit].keyed = true;
            unit_bound[active_unit].token = keyed_names[i].token;
            break;
        }
    }
    pthread_mutex_unlock(&keyed_lock);
}
/* a draw with a keyed picture bound: that picture is in the frame */
static void note_draw(void)
{
    pthread_mutex_lock(&keyed_lock);
    for (unsigned u = 0; u < UNITS; u++) {
        if (unit_bound[u].keyed) { drawn = unit_bound[u].token; have_drawn = true; break; }
    }
    pthread_mutex_unlock(&keyed_lock);
}
static void my_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    note_draw();
    glDrawArrays(mode, first, count);
}
static void my_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    note_draw();
    glDrawElements(mode, count, type, indices);
}
static BOOL (*present_o)(id, SEL, unsigned long);
static BOOL present(id self, SEL sel, unsigned long renderbuffer)
{
    struct twl_vplane_token token;
    bool show;
    pthread_mutex_lock(&keyed_lock);
    show = have_drawn;
    token = drawn;
    have_drawn = false;
    pthread_mutex_unlock(&keyed_lock);
    if (show && vplane >= 0) {
        /* the picture before still waiting for a refresh: until it is taken (one picture a refresh, none lost) */
        (void)ioctl(vplane, TWL_VPLANE_WAIT);
        bool shown = ioctl(vplane, TWL_VPLANE_SHOW, &token) == 0;
        /* decoded over already (ESTALE): the plane keeps the picture before */
        debug("present: slot %u generation %u: %s\n", token.slot, token.generation, shown ? "shown" : strerror(errno));
    }
    return present_o(self, sel, renderbuffer);
}
static void hook_present(void)
{
    static bool done;
    if (done) return;
    done = true;
    Class c = objc_getClass("EAGLContext");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("presentRenderbuffer:")) : NULL;
    if (m) present_o = (BOOL (*)(id, SEL, unsigned long))method_setImplementation(m, (IMP)present);
    debug("presentRenderbuffer: %s\n", m ? "hooked" : "not found");
}
static CFTypeID (*register_class)(const struct runtime_class *);
static CFTypeRef (*create_instance)(CFAllocatorRef, CFTypeID, CFIndex, unsigned char *);
static CFTypeID cache_type, texture_type;
static pthread_once_t once = PTHREAD_ONCE_INIT;
struct dead_texture { GLuint name; struct dead_texture *next; };
struct cache {
    struct runtime_base base;
    id context;
    pthread_mutex_t lock;
    struct dead_texture *dead;
};
struct texture {
    struct runtime_base base;
    struct cache *cache;
    CVPixelBufferRef source;
    struct dead_texture *deletion;
};

static id current_context(void)
{
    return ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("EAGLContext"), sel_registerName("currentContext"));
}
static bool set_context(id context)
{
    return ((BOOL (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("EAGLContext"),
        sel_registerName("setCurrentContext:"), context);
}
static void retain_object(id object)
{
    if (object) ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("retain"));
}
static void release_object(id object)
{
    if (object) ((void (*)(id, SEL))objc_msgSend)(object, sel_registerName("release"));
}
static void reap(struct cache *cache)
{
    if (current_context() != cache->context) return;
    pthread_mutex_lock(&cache->lock);
    struct dead_texture *dead = cache->dead;
    cache->dead = NULL;
    pthread_mutex_unlock(&cache->lock);
    while (dead) {
        struct dead_texture *next = dead->next;
        glDeleteTextures(1, &dead->name);
        free(dead);
        dead = next;
    }
}
static void finish_texture(CFTypeRef object)
{
    struct texture *texture = (void *)object;
    struct cache *cache = texture->cache;
    if (texture->source) CVPixelBufferRelease(texture->source);
    if (texture->deletion && texture->deletion->name) keyed_remove(texture->deletion->name);
    if (!cache) { free(texture->deletion); return; }
    pthread_mutex_lock(&cache->lock);
    texture->deletion->next = cache->dead;
    cache->dead = texture->deletion;
    pthread_mutex_unlock(&cache->lock);
    CFRelease(cache);
}
static void finish_cache(CFTypeRef object)
{
    struct cache *cache = (void *)object;
    id previous = current_context();
    retain_object(previous);
    if (set_context(cache->context)) {
        reap(cache);
        set_context(previous);
    }
    /* If the context is already unusable, GL storage dies with the context. */
    while (cache->dead) {
        struct dead_texture *next = cache->dead->next;
        free(cache->dead);
        cache->dead = next;
    }
    release_object(previous);
    release_object(cache->context);
    pthread_mutex_destroy(&cache->lock);
}
static const struct runtime_class cache_class = { .name = "TwlGLESTextureCache", .finalize = finish_cache };
static const struct runtime_class texture_class = { .name = "TwlGLESTexture", .finalize = finish_texture };
static void initialize(void)
{
    register_class = dlsym(RTLD_DEFAULT, "_CFRuntimeRegisterClass");
    create_instance = dlsym(RTLD_DEFAULT, "_CFRuntimeCreateInstance");
    if (register_class && create_instance) {
        cache_type = register_class(&cache_class);
        texture_type = register_class(&texture_class);
    }
}
static CFTypeID cache_id(void) { pthread_once(&once, initialize); return cache_type; }
static CFTypeID texture_id(void) { pthread_once(&once, initialize); return texture_type; }

static CVReturn cache_create(CFAllocatorRef allocator, CFDictionaryRef attributes, CVEAGLContext context,
                            CFDictionaryRef texture_attributes, CVOpenGLESTextureCacheRef *out)
{
    (void)attributes; (void)texture_attributes;
    if (!out) return kCVReturnInvalidArgument;
    *out = NULL;
    if (!context || !cache_id() || !texture_id()) return kCVReturnInvalidArgument;
    struct cache *cache = (void *)create_instance(allocator, cache_type,
        sizeof(*cache) - sizeof(cache->base), NULL);
    if (!cache) return kCVReturnAllocationFailed;
    pthread_mutex_init(&cache->lock, NULL);
    cache->context = context;
    retain_object(cache->context);
    *out = (void *)cache;
    fprintf(stderr, "twlcvgl: Mesa source-texture cache ready\n");
    return kCVReturnSuccess;
}
static void cache_flush(CVOpenGLESTextureCacheRef object, CVOptionFlags options)
{
    (void)options;
    if (object && CFGetTypeID(object) == cache_id()) reap((void *)object);
}
static CVReturn texture_create(CFAllocatorRef allocator, CVOpenGLESTextureCacheRef object,
    CVImageBufferRef source, CFDictionaryRef attributes, GLenum target, GLint internal,
    GLsizei width, GLsizei height, GLenum format, GLenum type, size_t plane, CVOpenGLESTextureRef *out)
{
    (void)attributes;
    if (!out) return kCVReturnInvalidArgument;
    *out = NULL;
    if (!object || CFGetTypeID(object) != cache_id() || !source ||
        CFGetTypeID(source) != CVPixelBufferGetTypeID()) return kCVReturnInvalidArgument;
    struct cache *cache = (void *)object;
    OSType pixel_format = CVPixelBufferGetPixelFormatType(source);
    if (current_context() != cache->context || target != GL_TEXTURE_2D || type != GL_UNSIGNED_BYTE ||
        (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
         pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
        CVPixelBufferGetPlaneCount(source) != 2 || plane > 1 || width <= 0 || height <= 0 ||
        (size_t)width != CVPixelBufferGetWidthOfPlane(source, plane) ||
        (size_t)height != CVPixelBufferGetHeightOfPlane(source, plane) ||
        format != (plane ? GL_LUMINANCE_ALPHA : GL_LUMINANCE) || internal != (GLint)format)
        return kCVReturnPixelBufferNotOpenGLCompatible;
    reap(cache);
    struct dead_texture *deletion = calloc(1, sizeof(*deletion));
    if (!deletion) return kCVReturnAllocationFailed;
    struct texture *texture = (void *)create_instance(allocator, texture_type,
        sizeof(*texture) - sizeof(texture->base), NULL);
    if (!texture) { free(deletion); return kCVReturnAllocationFailed; }
    texture->deletion = deletion;
    texture->cache = (void *)CFRetain(cache);
    texture->source = CVPixelBufferRetain(source);
    struct twl_vplane_token token;
    hook_present();
    if (plane_token(source, plane, &token)) {
        static const uint8_t key[2] = { 255, 255 }, luma = 60;
        GLint binding, alignment;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &deletion->name);
        glBindTexture(target, deletion->name);
        glTexImage2D(target, 0, internal, 1, 1, 0, format, type, plane ? (const void *)key : (const void *)&luma);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLenum error = glGetError();
        glBindTexture(target, (GLuint)binding);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        if (error || !deletion->name) { CFRelease(texture); return kCVReturnError; }
        if (!plane) keyed_add(deletion->name, &token);
        *out = (void *)texture;
        return kCVReturnSuccess;
    }
    CVReturn status = CVPixelBufferLockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
    if (status) { CFRelease(texture); return status; }
    size_t stride = CVPixelBufferGetBytesPerRowOfPlane(source, plane);
    unsigned bytes = plane ? 2 : 1;
    const void *base = CVPixelBufferGetBaseAddressOfPlane(source, plane);
    if (!base || stride % bytes || stride / bytes > INT32_MAX || stride < (size_t)width * bytes) {
        CVPixelBufferUnlockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
        CFRelease(texture); return kCVReturnInvalidArgument;
    }
    GLint binding, alignment, row_length, skip_rows, skip_pixels, unpack_buffer;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(stride / bytes));
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glGenTextures(1, &deletion->name);
    glBindTexture(target, deletion->name);
    glTexImage2D(target, 0, internal, width, height, 0, format, type, base);
    GLenum error = glGetError();
    glBindTexture(target, (GLuint)binding);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack_buffer);
    CVPixelBufferUnlockBaseAddress(source, kCVPixelBufferLock_ReadOnly);
    if (error || !deletion->name) {
        fprintf(stderr, "twlcvgl: plane upload GL error %#x\n", error);
        CFRelease(texture); return kCVReturnError;
    }
    *out = (void *)texture;
    return kCVReturnSuccess;
}
static GLuint texture_name(CVOpenGLESTextureRef object)
{
    return object && CFGetTypeID(object) == texture_id() ? ((struct texture *)object)->deletion->name : 0;
}
static GLenum texture_target(CVOpenGLESTextureRef object)
{
    return texture_name(object) ? GL_TEXTURE_2D : 0;
}
static Boolean texture_flipped(CVOpenGLESTextureRef object) { (void)object; return true; }
static void texture_coords(CVOpenGLESTextureRef object, GLfloat lower_left[2], GLfloat lower_right[2],
                           GLfloat upper_right[2], GLfloat upper_left[2])
{
    (void)object;
    lower_left[0] = 0; lower_left[1] = 1;
    lower_right[0] = 1; lower_right[1] = 1;
    upper_right[0] = 1; upper_right[1] = 0;
    upper_left[0] = 0; upper_left[1] = 0;
}
#define INTERPOSE(ours, theirs) \
    __attribute__((used, section("__DATA,__interpose"))) static const struct { \
        const void *replacement, *original; \
    } interpose_##theirs = { (const void *)(ours), (const void *)(theirs) }
INTERPOSE(cache_id, CVOpenGLESTextureCacheGetTypeID);
INTERPOSE(texture_id, CVOpenGLESTextureGetTypeID);
INTERPOSE(cache_create, CVOpenGLESTextureCacheCreate);
INTERPOSE(cache_flush, CVOpenGLESTextureCacheFlush);
INTERPOSE(texture_create, CVOpenGLESTextureCacheCreateTextureFromImage);
INTERPOSE(texture_name, CVOpenGLESTextureGetName);
INTERPOSE(texture_target, CVOpenGLESTextureGetTarget);
INTERPOSE(texture_flipped, CVOpenGLESTextureIsFlipped);
INTERPOSE(texture_coords, CVOpenGLESTextureGetCleanTexCoords);
INTERPOSE(my_bind_texture, glBindTexture);
INTERPOSE(my_active_texture, glActiveTexture);
INTERPOSE(my_draw_arrays, glDrawArrays);
INTERPOSE(my_draw_elements, glDrawElements);
