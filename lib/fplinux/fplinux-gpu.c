/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include "fplinux-gpu.h"

#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002

struct egl_functions {
	PFNEGLGETPROCADDRESSPROC get_proc_address;
	PFNEGLQUERYSTRINGPROC query_string;
	PFNEGLINITIALIZEPROC initialize;
	PFNEGLBINDAPIPROC bind_api;
	PFNEGLCHOOSECONFIGPROC choose_config;
	PFNEGLGETCONFIGATTRIBPROC get_config_attrib;
	PFNEGLCREATECONTEXTPROC create_context;
	PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer_surface;
	PFNEGLQUERYSURFACEPROC query_surface;
	PFNEGLMAKECURRENTPROC make_current;
	PFNEGLGETCURRENTCONTEXTPROC get_current_context;
	PFNEGLDESTROYSURFACEPROC destroy_surface;
	PFNEGLDESTROYCONTEXTPROC destroy_context;
	PFNEGLTERMINATEPROC terminate;
	PFNEGLGETERRORPROC get_error;
};

struct gl_functions {
	PFNGLGETSTRINGPROC get_string;
	PFNGLGETINTEGERVPROC get_integer;
	PFNGLGETERRORPROC get_error;
};

struct fplinux_gpu {
	void *library;
	struct egl_functions egl;
	struct gl_functions gl;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	bool initialized;
	int fd;
	dev_t device_id;
	char *device;
	char *renderer;
	char *version;
	unsigned int width;
	unsigned int height;
	char error[256];
};

static bool set_error(struct fplinux_gpu *gpu, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(gpu->error, sizeof(gpu->error), format, arguments);
	va_end(arguments);
	return false;
}

static bool egl_error(struct fplinux_gpu *gpu, const char *operation)
{
	return set_error(gpu, "%s failed: EGL error 0x%x", operation,
			 gpu->egl.get_error());
}

static bool has_extension(const char *extensions, const char *name)
{
	size_t length = strlen(name);
	const char *match = extensions;

	if (!extensions)
		return false;
	while ((match = strstr(match, name))) {
		if ((match == extensions || match[-1] == ' ') &&
		    (match[length] == '\0' || match[length] == ' '))
			return true;
		match += length;
	}
	return false;
}

#define LOAD_EGL(member, type, name)                                           \
	do {                                                                   \
		gpu->egl.member = (type)dlsym(gpu->library, name);             \
		if (!gpu->egl.member)                                          \
			return set_error(                                      \
				gpu, "EGL entry point unavailable: %s", name); \
	} while (0)

static bool load_egl(struct fplinux_gpu *gpu)
{
	gpu->library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!gpu->library)
		return set_error(gpu, "EGL unavailable: %s", dlerror());
	LOAD_EGL(get_proc_address, PFNEGLGETPROCADDRESSPROC,
		 "eglGetProcAddress");
	LOAD_EGL(query_string, PFNEGLQUERYSTRINGPROC, "eglQueryString");
	LOAD_EGL(initialize, PFNEGLINITIALIZEPROC, "eglInitialize");
	LOAD_EGL(bind_api, PFNEGLBINDAPIPROC, "eglBindAPI");
	LOAD_EGL(choose_config, PFNEGLCHOOSECONFIGPROC, "eglChooseConfig");
	LOAD_EGL(get_config_attrib, PFNEGLGETCONFIGATTRIBPROC,
		 "eglGetConfigAttrib");
	LOAD_EGL(create_context, PFNEGLCREATECONTEXTPROC, "eglCreateContext");
	LOAD_EGL(create_pbuffer_surface, PFNEGLCREATEPBUFFERSURFACEPROC,
		 "eglCreatePbufferSurface");
	LOAD_EGL(query_surface, PFNEGLQUERYSURFACEPROC, "eglQuerySurface");
	LOAD_EGL(make_current, PFNEGLMAKECURRENTPROC, "eglMakeCurrent");
	LOAD_EGL(get_current_context, PFNEGLGETCURRENTCONTEXTPROC,
		 "eglGetCurrentContext");
	LOAD_EGL(destroy_surface, PFNEGLDESTROYSURFACEPROC,
		 "eglDestroySurface");
	LOAD_EGL(destroy_context, PFNEGLDESTROYCONTEXTPROC,
		 "eglDestroyContext");
	LOAD_EGL(terminate, PFNEGLTERMINATEPROC, "eglTerminate");
	LOAD_EGL(get_error, PFNEGLGETERRORPROC, "eglGetError");
	return true;
}

#undef LOAD_EGL

static int open_render_node(const char *path, dev_t *device)
{
	drmVersionPtr version;
	struct stat status;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	bool supported;

	if (fd < 0)
		return -1;
	if (fstat(fd, &status) || !S_ISCHR(status.st_mode) ||
	    drmGetNodeTypeFromFd(fd) != DRM_NODE_RENDER) {
		close(fd);
		errno = ENODEV;
		return -1;
	}
	version = drmGetVersion(fd);
	supported = version && version->name && version->name_len == 8 &&
		    !memcmp(version->name, "panfrost", 8);
	drmFreeVersion(version);
	if (!supported) {
		close(fd);
		errno = ENODEV;
		return -1;
	}
	*device = status.st_rdev;
	return fd;
}

static bool select_device(struct fplinux_gpu *gpu, const char *path)
{
	drmDevicePtr *devices;
	int capacity;
	int count;
	bool result = false;

	if (path) {
		gpu->fd = open_render_node(path, &gpu->device_id);
		if (gpu->fd < 0)
			return set_error(gpu,
					 "open Panfrost render node %s: %s",
					 path, strerror(errno));
		gpu->device = strdup(path);
		if (!gpu->device)
			return set_error(gpu, "allocate render node path: %s",
					 strerror(errno));
		return true;
	}
	capacity = drmGetDevices2(0, NULL, 0);
	if (capacity <= 0)
		return set_error(gpu, "no supported Panfrost render node");
	devices = calloc((size_t)capacity, sizeof(*devices));
	if (!devices)
		return set_error(gpu, "allocate DRM device list: %s",
				 strerror(errno));
	count = drmGetDevices2(0, devices, capacity);
	if (count < 0 || count > capacity) {
		set_error(gpu, "enumerate DRM devices failed");
		goto out;
	}
	for (int i = 0; i < count; i++) {
		dev_t device;
		int fd;

		if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;
		path = devices[i]->nodes[DRM_NODE_RENDER];
		fd = open_render_node(path, &device);
		if (fd < 0)
			continue;
		if (gpu->fd >= 0) {
			close(fd);
			if (device == gpu->device_id)
				continue;
			set_error(gpu,
				  "multiple supported Panfrost render nodes");
			goto out;
		}
		gpu->fd = fd;
		gpu->device_id = device;
		gpu->device = strdup(path);
		if (!gpu->device) {
			set_error(gpu, "allocate render node path: %s",
				  strerror(errno));
			goto out;
		}
	}
	if (gpu->fd < 0)
		set_error(gpu, "no supported Panfrost render node");
	else
		result = true;
out:
	if (count >= 0 && count <= capacity)
		drmFreeDevices(devices, count);
	free(devices);
	return result;
}

static bool find_display(struct fplinux_gpu *gpu)
{
	PFNEGLQUERYDEVICESEXTPROC query_devices =
		(PFNEGLQUERYDEVICESEXTPROC)gpu->egl.get_proc_address(
			"eglQueryDevicesEXT");
	PFNEGLQUERYDEVICESTRINGEXTPROC query_string =
		(PFNEGLQUERYDEVICESTRINGEXTPROC)gpu->egl.get_proc_address(
			"eglQueryDeviceStringEXT");
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)gpu->egl.get_proc_address(
			"eglGetPlatformDisplayEXT");
	EGLDeviceEXT *devices;
	EGLint capacity = 0;
	EGLint count = 0;
	bool result = false;

	if (!query_devices || !query_string || !get_display ||
	    !has_extension(gpu->egl.query_string(EGL_NO_DISPLAY,
						 EGL_EXTENSIONS),
			   "EGL_EXT_platform_device"))
		return set_error(gpu, "EGL device platform unavailable");
	if (!query_devices(0, NULL, &capacity) || capacity <= 0)
		return set_error(gpu, "no EGL devices");
	devices = calloc((size_t)capacity, sizeof(*devices));
	if (!devices)
		return set_error(gpu, "allocate EGL device list: %s",
				 strerror(errno));
	if (!query_devices(capacity, devices, &count) || count < 0 ||
	    count > capacity) {
		egl_error(gpu, "eglQueryDevicesEXT");
		goto out;
	}
	for (EGLint i = 0; i < count; i++) {
		const char *extensions =
			query_string(devices[i], EGL_EXTENSIONS);
		const char *path;
		struct stat status;

		if (!has_extension(extensions,
				   "EGL_EXT_device_drm_render_node") ||
		    has_extension(extensions, "EGL_MESA_device_software"))
			continue;
		path = query_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
		if (!path || stat(path, &status) || !S_ISCHR(status.st_mode) ||
		    status.st_rdev != gpu->device_id)
			continue;
		gpu->display =
			get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
		if (gpu->display != EGL_NO_DISPLAY) {
			result = true;
			break;
		}
	}
	if (!result)
		set_error(gpu, "no EGL hardware device matches %s",
			  gpu->device);
out:
	free(devices);
	return result;
}

static bool config_matches(struct fplinux_gpu *gpu, EGLConfig config)
{
	static const EGLint colors[] = {
		EGL_RED_SIZE,
		EGL_GREEN_SIZE,
		EGL_BLUE_SIZE,
		EGL_ALPHA_SIZE,
	};
	EGLint value;
	EGLint limit_width;
	EGLint limit_height;

	for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
		if (!gpu->egl.get_config_attrib(gpu->display, config, colors[i],
						&value) ||
		    value != 8)
			return false;
	}
	if (!gpu->egl.get_config_attrib(gpu->display, config, EGL_DEPTH_SIZE,
					&value) ||
	    value < 16 ||
	    !gpu->egl.get_config_attrib(gpu->display, config,
					EGL_MAX_PBUFFER_WIDTH, &limit_width) ||
	    !gpu->egl.get_config_attrib(gpu->display, config,
					EGL_MAX_PBUFFER_HEIGHT, &limit_height))
		return false;
	return limit_width > 0 && limit_height > 0 &&
	       gpu->width <= (unsigned int)limit_width &&
	       gpu->height <= (unsigned int)limit_height;
}

static bool choose_config(struct fplinux_gpu *gpu, enum fplinux_gpu_api api,
			  EGLConfig *config)
{
	const EGLint attributes[] = {
		EGL_SURFACE_TYPE,
		EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE,
		api == FPLINUX_GPU_GLES3 ? EGL_OPENGL_ES3_BIT_KHR :
					   EGL_OPENGL_BIT,
		EGL_RED_SIZE,
		8,
		EGL_GREEN_SIZE,
		8,
		EGL_BLUE_SIZE,
		8,
		EGL_ALPHA_SIZE,
		8,
		EGL_DEPTH_SIZE,
		16,
		EGL_NONE,
	};
	EGLConfig *configs;
	EGLint capacity = 0;
	EGLint count = 0;
	bool result = false;

	if (!gpu->egl.choose_config(gpu->display, attributes, NULL, 0,
				    &capacity) ||
	    capacity <= 0)
		return set_error(gpu, "no RGBA8 depth16 EGL configuration");
	configs = calloc((size_t)capacity, sizeof(*configs));
	if (!configs)
		return set_error(gpu, "allocate EGL configurations: %s",
				 strerror(errno));
	if (!gpu->egl.choose_config(gpu->display, attributes, configs, capacity,
				    &count) ||
	    count < 0 || count > capacity) {
		egl_error(gpu, "eglChooseConfig");
		goto out;
	}
	for (EGLint i = 0; i < count; i++) {
		if (!config_matches(gpu, configs[i]))
			continue;
		*config = configs[i];
		result = true;
		break;
	}
	if (!result)
		set_error(gpu, "no RGBA8 depth16 EGL configuration for %ux%u",
			  gpu->width, gpu->height);
out:
	free(configs);
	return result;
}

static bool initialize_context(struct fplinux_gpu *gpu,
			       enum fplinux_gpu_api api)
{
	const EGLint gles_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION,
		3,
		EGL_NONE,
	};
	const EGLint desktop_attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR,
		2,
		EGL_CONTEXT_MINOR_VERSION_KHR,
		1,
		EGL_NONE,
	};
	const EGLint surface_attributes[] = {
		EGL_WIDTH,	     (EGLint)gpu->width, EGL_HEIGHT,
		(EGLint)gpu->height, EGL_NONE,
	};
	EGLConfig config = NULL;
	EGLint width;
	EGLint height;

	if (!find_display(gpu))
		return false;
	if (!gpu->egl.initialize(gpu->display, NULL, NULL))
		return egl_error(gpu, "eglInitialize");
	gpu->initialized = true;
	if (api == FPLINUX_GPU_OPENGL2 &&
	    !has_extension(gpu->egl.query_string(gpu->display, EGL_EXTENSIONS),
			   "EGL_KHR_create_context"))
		return set_error(gpu,
				 "EGL desktop context creation unavailable");
	if (!gpu->egl.bind_api(api == FPLINUX_GPU_GLES3 ? EGL_OPENGL_ES_API :
							  EGL_OPENGL_API))
		return egl_error(gpu, "eglBindAPI");
	if (!choose_config(gpu, api, &config))
		return false;
	gpu->context = gpu->egl.create_context(gpu->display, config,
					       EGL_NO_CONTEXT,
					       api == FPLINUX_GPU_GLES3 ?
						       gles_attributes :
						       desktop_attributes);
	if (gpu->context == EGL_NO_CONTEXT)
		return egl_error(gpu, "eglCreateContext");
	gpu->surface = gpu->egl.create_pbuffer_surface(gpu->display, config,
						       surface_attributes);
	if (gpu->surface == EGL_NO_SURFACE)
		return egl_error(gpu, "eglCreatePbufferSurface");
	if (!gpu->egl.query_surface(gpu->display, gpu->surface, EGL_WIDTH,
				    &width) ||
	    !gpu->egl.query_surface(gpu->display, gpu->surface, EGL_HEIGHT,
				    &height) ||
	    width != (EGLint)gpu->width || height != (EGLint)gpu->height)
		return set_error(gpu, "EGL pbuffer dimensions do not match");
	if (!gpu->egl.make_current(gpu->display, gpu->surface, gpu->surface,
				   gpu->context))
		return egl_error(gpu, "eglMakeCurrent");
	return true;
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name)
{
	void *procedure;

	if (!gpu || !name || !*name)
		return NULL;
	procedure = (void *)gpu->egl.get_proc_address(name);
	if (!procedure)
		procedure = dlsym(gpu->library, name);
	if (!procedure)
		set_error(gpu, "GL entry point unavailable: %s", name);
	return procedure;
}

#define LOAD_GL(member, type, name)                                     \
	do {                                                            \
		gpu->gl.member = (type)fplinux_gpu_get_proc(gpu, name); \
		if (!gpu->gl.member)                                    \
			return false;                                   \
	} while (0)

static bool initialize_gl(struct fplinux_gpu *gpu, enum fplinux_gpu_api api)
{
	PFNEGLGETDISPLAYDRIVERNAMEPROC driver_name;
	const char *driver;
	const char *renderer;
	const char *version;
	unsigned int major;
	unsigned int minor;
	GLint profile = 0;
	GLint viewport[2] = { 0, 0 };

	LOAD_GL(get_string, PFNGLGETSTRINGPROC, "glGetString");
	LOAD_GL(get_integer, PFNGLGETINTEGERVPROC, "glGetIntegerv");
	LOAD_GL(get_error, PFNGLGETERRORPROC, "glGetError");
	if (!has_extension(gpu->egl.query_string(gpu->display, EGL_EXTENSIONS),
			   "EGL_MESA_query_driver"))
		return set_error(gpu, "EGL driver identity unavailable");
	driver_name = (PFNEGLGETDISPLAYDRIVERNAMEPROC)gpu->egl.get_proc_address(
		"eglGetDisplayDriverName");
	driver = driver_name ? driver_name(gpu->display) : NULL;
	renderer = (const char *)gpu->gl.get_string(GL_RENDERER);
	version = (const char *)gpu->gl.get_string(GL_VERSION);
	if (!driver || strcmp(driver, "panfrost") || !renderer ||
	    !strstr(renderer, "(Panfrost)") || strstr(renderer, "llvmpipe") ||
	    strstr(renderer, "softpipe"))
		return set_error(gpu,
				 "expected Panfrost, driver=%s renderer=%s",
				 driver ? driver : "(none)",
				 renderer ? renderer : "(none)");
	if (!version)
		return set_error(gpu, "GL version unavailable");
	if (api == FPLINUX_GPU_GLES3) {
		if (sscanf(version, "OpenGL ES %u.%u", &major, &minor) != 2 ||
		    major < 3)
			return set_error(gpu, "expected OpenGL ES 3, got %s",
					 version);
	} else {
		if (sscanf(version, "%u.%u", &major, &minor) != 2 ||
		    major < 2 || (major == 2 && minor < 1))
			return set_error(gpu,
					 "expected desktop OpenGL 2.1, got %s",
					 version);
		if (major > 3 || (major == 3 && minor >= 2)) {
			gpu->gl.get_integer(GL_CONTEXT_PROFILE_MASK, &profile);
			if (!(profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT))
				return set_error(
					gpu,
					"desktop OpenGL compatibility profile unavailable");
		} else if (major == 3 && minor == 1 &&
			   !has_extension((const char *)gpu->gl.get_string(
						  GL_EXTENSIONS),
					  "GL_ARB_compatibility")) {
			return set_error(
				gpu,
				"desktop OpenGL compatibility unavailable");
		}
	}
	gpu->gl.get_integer(GL_MAX_VIEWPORT_DIMS, viewport);
	if (gpu->gl.get_error() != GL_NO_ERROR || viewport[0] <= 0 ||
	    viewport[1] <= 0 || gpu->width > (unsigned int)viewport[0] ||
	    gpu->height > (unsigned int)viewport[1])
		return set_error(gpu, "GL viewport cannot contain %ux%u",
				 gpu->width, gpu->height);
	gpu->renderer = strdup(renderer);
	gpu->version = strdup(version);
	if (!gpu->renderer || !gpu->version)
		return set_error(gpu, "allocate GL identity: %s",
				 strerror(errno));
	return true;
}

#undef LOAD_GL

struct fplinux_gpu *fplinux_gpu_open(enum fplinux_gpu_api api,
				     unsigned int width, unsigned int height,
				     const char *device, char *error,
				     size_t error_size)
{
	struct fplinux_gpu *gpu;

	if (error && error_size)
		error[0] = '\0';
	gpu = calloc(1, sizeof(*gpu));
	if (!gpu) {
		if (error && error_size)
			snprintf(error, error_size, "allocate GPU context: %s",
				 strerror(errno));
		return NULL;
	}
	gpu->fd = -1;
	gpu->display = EGL_NO_DISPLAY;
	gpu->context = EGL_NO_CONTEXT;
	gpu->surface = EGL_NO_SURFACE;
	gpu->width = width;
	gpu->height = height;
	if ((api != FPLINUX_GPU_GLES3 && api != FPLINUX_GPU_OPENGL2) ||
	    !width || !height || width > INT_MAX || height > INT_MAX) {
		set_error(gpu, "invalid GPU API or framebuffer dimensions");
		goto fail;
	}
	if (!load_egl(gpu) || !select_device(gpu, device) ||
	    !initialize_context(gpu, api) || !initialize_gl(gpu, api))
		goto fail;
	return gpu;
fail:
	if (error && error_size)
		snprintf(error, error_size, "%s", gpu->error);
	fplinux_gpu_close(gpu);
	return NULL;
}

const char *fplinux_gpu_device(const struct fplinux_gpu *gpu)
{
	return gpu ? gpu->device : NULL;
}

const char *fplinux_gpu_renderer(const struct fplinux_gpu *gpu)
{
	return gpu ? gpu->renderer : NULL;
}

const char *fplinux_gpu_version(const struct fplinux_gpu *gpu)
{
	return gpu ? gpu->version : NULL;
}

void fplinux_gpu_close(struct fplinux_gpu *gpu)
{
	if (!gpu)
		return;
	free(gpu->version);
	free(gpu->renderer);
	if (gpu->context != EGL_NO_CONTEXT &&
	    gpu->egl.get_current_context() == gpu->context)
		gpu->egl.make_current(gpu->display, EGL_NO_SURFACE,
				      EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (gpu->surface != EGL_NO_SURFACE)
		gpu->egl.destroy_surface(gpu->display, gpu->surface);
	if (gpu->context != EGL_NO_CONTEXT)
		gpu->egl.destroy_context(gpu->display, gpu->context);
	if (gpu->initialized)
		gpu->egl.terminate(gpu->display);
	free(gpu->device);
	if (gpu->fd >= 0)
		close(gpu->fd);
	if (gpu->library)
		dlclose(gpu->library);
	free(gpu);
}
