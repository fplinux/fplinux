/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>

#include "fplinux-gpu.h"

#define TEST_DISPLAY ((EGLDisplay)(uintptr_t)1)
#define TEST_CONTEXT ((EGLContext)(uintptr_t)2)
#define TEST_SURFACE ((EGLSurface)(uintptr_t)3)
#define TEST_CONFIG ((EGLConfig)(uintptr_t)4)

static const char *scenario;
static bool library_live;
static bool initialized;
static bool context_live;
static bool surface_live;
static unsigned int descriptors;
static EGLenum bound_api;
static EGLContext current_context;

static bool is_scenario(const char *name)
{
	return !strcmp(scenario, name);
}

static EGLint attribute_value(const EGLint *attributes, EGLint name)
{
	for (size_t i = 0; attributes[i] != EGL_NONE; i += 2)
		if (attributes[i] == name)
			return attributes[i + 1];
	assert(0 && "missing EGL attribute");
	return 0;
}

static void expect_released(void)
{
	assert(!library_live && !initialized && !context_live && !surface_live);
	assert(!descriptors && current_context == EGL_NO_CONTEXT);
}

int __wrap_open(const char *path, int flags, ...)
{
	int fd;

	assert(flags == (O_RDWR | O_CLOEXEC));
	if (!strcmp(path, "/dev/dri/renderD131"))
		fd = 101;
	else if (!strcmp(path, "/dev/dri/renderD132"))
		fd = 102;
	else if (!strcmp(path, "/dev/dri/renderD130"))
		fd = 103;
	else {
		errno = ENOENT;
		return -1;
	}
	descriptors++;
	return fd;
}

int __wrap_close(int fd)
{
	assert(fd >= 101 && fd <= 103 && descriptors);
	descriptors--;
	return 0;
}

int __wrap_fstat(int fd, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_mode = S_IFCHR | 0600;
	status->st_rdev = makedev(226, fd == 102 ? 132 : 131);
	return 0;
}

int __wrap_stat(const char *path, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_mode = S_IFCHR | 0600;
	status->st_rdev = makedev(226, !strcmp(path, "/egl/wrong") ? 132 : 131);
	return 0;
}

int __wrap_drmGetNodeTypeFromFd(int fd)
{
	assert(fd >= 101 && fd <= 103);
	return DRM_NODE_RENDER;
}

drmVersionPtr __wrap_drmGetVersion(int fd)
{
	static drmVersion panfrost = { .name_len = 8, .name = "panfrost" };
	static drmVersion unrelated = { .name_len = 5, .name = "other" };

	return fd == 103 ? &unrelated : &panfrost;
}

void __wrap_drmFreeVersion(drmVersionPtr version)
{
	assert(version);
}

int __wrap_drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int maximum)
{
	static char *nodes[][DRM_NODE_MAX] = {
		{ "/dev/dri/card5", NULL, NULL },
		{ NULL, NULL, "/dev/dri/renderD130" },
		{ NULL, NULL, "/dev/dri/renderD131" },
		{ NULL, NULL, "/dev/dri/renderD132" },
	};
	static drmDevice available[4];
	int count = is_scenario("ambiguous") ? 4 : 3;

	assert(!flags);
	if (!devices)
		return count;
	assert(maximum >= count);
	for (int i = 0; i < count; i++) {
		available[i].nodes = nodes[i];
		available[i].available_nodes =
			1 << (i ? DRM_NODE_RENDER : DRM_NODE_PRIMARY);
		devices[i] = &available[i];
	}
	return count;
}

void __wrap_drmFreeDevices(drmDevicePtr devices[], int count)
{
	assert(devices && count >= 3);
}

EGLBoolean eglQueryDevicesEXT(EGLint maximum, EGLDeviceEXT *devices,
			      EGLint *count)
{
	*count = 3;
	if (maximum) {
		assert(maximum >= 3);
		for (uintptr_t i = 0; i < 3; i++)
			devices[i] = (EGLDeviceEXT)(i + 1);
	}
	return EGL_TRUE;
}

const char *eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name)
{
	uintptr_t index = (uintptr_t)device;

	if (name == EGL_EXTENSIONS)
		return index == 1 || is_scenario("software-device") ?
			       "EGL_EXT_device_drm_render_node EGL_MESA_device_software" :
			       "EGL_EXT_device_drm_render_node";
	assert(name == EGL_DRM_RENDER_NODE_FILE_EXT);
	return index == 2 || is_scenario("wrong-device") ? "/egl/wrong" :
							   "/egl/matched";
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *device,
				    const EGLint *attributes)
{
	assert(platform == EGL_PLATFORM_DEVICE_EXT && !attributes);
	assert((uintptr_t)device == 3);
	return TEST_DISPLAY;
}

const char *eglQueryString(EGLDisplay display, EGLint name)
{
	assert(name == EGL_EXTENSIONS);
	return display == EGL_NO_DISPLAY ?
		       "EGL_EXT_platform_device" :
		       "EGL_MESA_query_driver EGL_KHR_create_context";
}

EGLBoolean eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor)
{
	assert(display == TEST_DISPLAY && !major && !minor);
	initialized = true;
	return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
	bound_api = api;
	return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint *attributes,
			   EGLConfig *configs, EGLint maximum, EGLint *count)
{
	assert(display == TEST_DISPLAY && attributes);
	*count = 1;
	if (maximum)
		configs[0] = TEST_CONFIG;
	return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
			      EGLint attribute, EGLint *value)
{
	assert(display == TEST_DISPLAY && config == TEST_CONFIG);
	switch (attribute) {
	case EGL_RED_SIZE:
	case EGL_GREEN_SIZE:
	case EGL_BLUE_SIZE:
	case EGL_ALPHA_SIZE:
		*value = 8;
		break;
	case EGL_DEPTH_SIZE:
		*value = 16;
		break;
	case EGL_MAX_PBUFFER_WIDTH:
		*value = is_scenario("pbuffer-width-limit") ? 1 : 1024;
		break;
	case EGL_MAX_PBUFFER_HEIGHT:
		*value = is_scenario("pbuffer-height-limit") ? 2 : 1024;
		break;
	case EGL_MAX_PBUFFER_PIXELS:
		*value = 0;
		break;
	default:
		assert(0 && "unexpected EGL configuration attribute");
	}
	return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config,
			    EGLContext shared, const EGLint *attributes)
{
	assert(display == TEST_DISPLAY && config == TEST_CONFIG && !shared);
	if (bound_api == EGL_OPENGL_API) {
		assert(attribute_value(attributes,
				       EGL_CONTEXT_MAJOR_VERSION_KHR) == 2);
		assert(attribute_value(attributes,
				       EGL_CONTEXT_MINOR_VERSION_KHR) == 1);
	} else {
		assert(bound_api == EGL_OPENGL_ES_API);
		assert(attribute_value(attributes,
				       EGL_CONTEXT_CLIENT_VERSION) == 3);
	}
	context_live = true;
	return TEST_CONTEXT;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
				   const EGLint *attributes)
{
	assert(display == TEST_DISPLAY && config == TEST_CONFIG);
	assert(attribute_value(attributes, EGL_WIDTH) == 2);
	assert(attribute_value(attributes, EGL_HEIGHT) == 3);
	if (is_scenario("surface-failure"))
		return EGL_NO_SURFACE;
	surface_live = true;
	return TEST_SURFACE;
}

EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint name,
			   EGLint *value)
{
	assert(display == TEST_DISPLAY && surface == TEST_SURFACE);
	assert(name == EGL_WIDTH || name == EGL_HEIGHT);
	*value = name == EGL_WIDTH ? 2 : 3;
	return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
			  EGLContext context)
{
	assert(display == TEST_DISPLAY);
	if (is_scenario("make-current-failure") && context != EGL_NO_CONTEXT)
		return EGL_FALSE;
	assert(draw == read);
	assert(context == EGL_NO_CONTEXT || draw == TEST_SURFACE);
	current_context = context;
	return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void)
{
	return current_context;
}

EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface)
{
	assert(library_live && display == TEST_DISPLAY &&
	       surface == TEST_SURFACE);
	assert(current_context == EGL_NO_CONTEXT);
	surface_live = false;
	return EGL_TRUE;
}

EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context)
{
	assert(library_live && display == TEST_DISPLAY &&
	       context == TEST_CONTEXT);
	assert(current_context == EGL_NO_CONTEXT);
	context_live = false;
	return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay display)
{
	assert(library_live && display == TEST_DISPLAY);
	assert(!surface_live && !context_live);
	initialized = false;
	return EGL_TRUE;
}

EGLint eglGetError(void)
{
	return EGL_BAD_ALLOC;
}

const char *eglGetDisplayDriverName(EGLDisplay display)
{
	assert(display == TEST_DISPLAY);
	return "panfrost";
}

const GLubyte *glGetString(GLenum name)
{
	const char *value;

	if (name == GL_RENDERER)
		value = is_scenario("software-renderer") ?
				"llvmpipe" :
				"Mali-T820 (Panfrost)";
	else {
		assert(name == GL_VERSION);
		value = bound_api == EGL_OPENGL_ES_API ?
				"OpenGL ES 3.1 Mesa" :
			is_scenario("desktop-core") ?
				"4.5 (Core Profile) Mesa" :
				"2.1 Mesa";
	}
	return (const GLubyte *)value;
}

void glGetIntegerv(GLenum name, GLint *value)
{
	if (name == GL_MAX_VIEWPORT_DIMS) {
		value[0] = 1024;
		value[1] = 1024;
	} else {
		assert(name == 0x9126);
		*value = 1;
	}
}

GLenum glGetError(void)
{
	return GL_NO_ERROR;
}

static void *resolve(const char *name)
{
#define RESOLVE(symbol)             \
	if (!strcmp(name, #symbol)) \
	return (void *)symbol
	RESOLVE(eglGetProcAddress);
	RESOLVE(eglQueryDevicesEXT);
	RESOLVE(eglQueryDeviceStringEXT);
	RESOLVE(eglGetPlatformDisplayEXT);
	RESOLVE(eglQueryString);
	RESOLVE(eglInitialize);
	RESOLVE(eglBindAPI);
	RESOLVE(eglChooseConfig);
	RESOLVE(eglGetConfigAttrib);
	RESOLVE(eglCreateContext);
	RESOLVE(eglCreatePbufferSurface);
	RESOLVE(eglQuerySurface);
	RESOLVE(eglMakeCurrent);
	RESOLVE(eglGetCurrentContext);
	RESOLVE(eglDestroySurface);
	RESOLVE(eglDestroyContext);
	RESOLVE(eglTerminate);
	RESOLVE(eglGetError);
	RESOLVE(eglGetDisplayDriverName);
	RESOLVE(glGetString);
	RESOLVE(glGetIntegerv);
	RESOLVE(glGetError);
#undef RESOLVE
	return NULL;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
	assert(library_live);
	return (__eglMustCastToProperFunctionPointerType)resolve(name);
}

void *__wrap_dlopen(const char *path, int flags)
{
	assert(!strcmp(path, "libEGL.so.1") &&
	       flags == (RTLD_NOW | RTLD_LOCAL));
	if (is_scenario("missing-provider"))
		return NULL;
	library_live = true;
	return (void *)(uintptr_t)1;
}

void *__wrap_dlsym(void *library, const char *name)
{
	assert(library && library_live);
	if (is_scenario("missing-procedure") && !strcmp(name, "eglMakeCurrent"))
		return NULL;
	return resolve(name);
}

char *__wrap_dlerror(void)
{
	return "test provider unavailable";
}

int __wrap_dlclose(void *library)
{
	assert(library && library_live);
	assert(!initialized && !context_live && !surface_live && !descriptors);
	library_live = false;
	return 0;
}

int main(int argc, char **argv)
{
	struct fplinux_gpu *gpu;
	char error[256];
	bool desktop;
	bool success;

	assert(argc == 2);
	scenario = argv[1];
	desktop = is_scenario("desktop") || is_scenario("desktop-core");
	if (is_scenario("invalid-dimensions")) {
		assert(!fplinux_gpu_open(FPLINUX_GPU_GLES3, 0, 3, NULL, error,
					 sizeof(error)));
		assert(!fplinux_gpu_open(FPLINUX_GPU_GLES3, UINT32_MAX,
					 UINT32_MAX, NULL, error,
					 sizeof(error)));
		expect_released();
		return 0;
	}
	gpu = fplinux_gpu_open(
		desktop ? FPLINUX_GPU_OPENGL2 : FPLINUX_GPU_GLES3, 2, 3,
		is_scenario("automatic") || is_scenario("ambiguous") ?
			NULL :
			"/dev/dri/renderD131",
		error, sizeof(error));
	success = is_scenario("gles") || is_scenario("desktop") ||
		  is_scenario("automatic");
	if (!success) {
		assert(!gpu && error[0]);
		expect_released();
		return 0;
	}
	assert(gpu && !error[0]);
	assert(!strcmp(fplinux_gpu_device(gpu), "/dev/dri/renderD131"));
	assert(!strcmp(fplinux_gpu_renderer(gpu), "Mali-T820 (Panfrost)"));
	assert(!strcmp(fplinux_gpu_version(gpu),
		       desktop ? "2.1 Mesa" : "OpenGL ES 3.1 Mesa"));
	assert(fplinux_gpu_get_proc(gpu, "glGetString"));
	assert(!fplinux_gpu_get_proc(gpu, "glMissingProcedure"));
	fplinux_gpu_close(gpu);
	expect_released();
	return 0;
}
