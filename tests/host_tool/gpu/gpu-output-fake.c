/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"
#include "gpu-output-fake.h"

#define GL_COLOR_LOGIC_OP 0x0bf2
#define GL_FRAMEBUFFER_SRGB 0x8db9
#define GL_ALPHA_TEST 0x0bc0
#define GL_POLYGON_MODE 0x0b40
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FILL 0x1b02

struct gpu_output_fake fake;
uint8_t mapping[192];
struct fplinux_drm_session session;
static struct {
	GLuint next_id;
	GLuint page_framebuffer[2];
} internal;

void reset(enum failure failure)
{
	memset(&fake, 0, sizeof(fake));
	memset(&internal, 0, sizeof(internal));
	fake.failure = failure;
	internal.next_id = 10;
	fake.exported_fd = -1;
	fake.state.active_texture = GL_TEXTURE0;
	memset(fake.state.mask, GL_TRUE, sizeof(fake.state.mask));
	fake.state.polygon_mode[0] = GL_FILL;
	fake.state.polygon_mode[1] = GL_FILL;
	memset(&session, 0, sizeof(session));
	session.mapping = mapping;
	session.size = sizeof(mapping);
	session.page_bytes = 96;
	session.stride = 20;
	session.width = 6;
	session.height = 4;
	session.format = DRM_FORMAT_RGB565;
	session.pages = 2;
	session.shown_page = 1;
	session.present_fence = -1;
	session.active = true;
}

static GLuint allocate(bool *objects)
{
	GLuint id = internal.next_id++;

	assert(id < 256);
	assert(!objects[id]);
	objects[id] = true;
	return id;
}

static void delete_objects(GLsizei count, const GLuint *ids, bool *objects)
{
	assert(session.present_fence < 0);
	for (GLsizei i = 0; i < count; ++i) {
		assert(ids[i] < 256 && objects[ids[i]]);
		objects[ids[i]] = false;
	}
}

struct fplinux_gpu *fplinux_gpu_open(enum fplinux_gpu_api api,
				     unsigned int width, unsigned int height,
				     const char *device, char *error,
				     size_t error_size)
{
	(void)error;
	(void)error_size;
	assert(width == 8 && height == 12 && !device && !fake.gpu.live);
	fake.gpu.live = true;
	fake.gpu.desktop = api == FPLINUX_GPU_OPENGL2;
	return &fake.gpu;
}

const char *fplinux_gpu_version(const struct fplinux_gpu *gpu)
{
	assert(gpu == &fake.gpu && gpu->live);
	return gpu->desktop ? "3.1 test" : "OpenGL ES 3.0 test";
}

void fplinux_gpu_close(struct fplinux_gpu *gpu)
{
	if (!gpu)
		return;
	assert(gpu == &fake.gpu && gpu->live);
	gpu->live = false;
	fake.closes++;
}

bool fplinux_drm_session_acquire_buffer(struct fplinux_drm_session *display,
					int *fd)
{
	assert(display == &session && !display->buffer_acquired);
	fake.exported_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(fake.exported_fd >= 0);
	*fd = fake.exported_fd;
	display->buffer_acquired = true;
	return true;
}

void fplinux_drm_session_release_buffer(struct fplinux_drm_session *display)
{
	assert(display == &session && display->buffer_acquired);
	assert(fake.exported_fd == -1);
	assert(display->present_fence < 0);
	display->buffer_acquired = false;
	fake.releases++;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *display)
{
	assert(display == &session);
	fake.dispatches++;
	return true;
}

bool fplinux_drm_session_queue_present(struct fplinux_drm_session *display,
				       unsigned int page)
{
	assert(display == &session && display->buffer_acquired);
	assert(fake.fence_complete && !fake.cpu_access && !fake.fence_live);
	assert(page < 2 && page != display->shown_page);
	assert(display->present_fence < 0 && !display->presentation_error);
	fake.present_calls++;
	fake.last_present_page = page;
	if (fake.failure == PRESENT) {
		errno = ENODEV;
		return false;
	}
	display->pending_page = page;
	display->present_fence = 7;
	return true;
}

bool fplinux_drm_session_wait_present(struct fplinux_drm_session *display)
{
	assert(display == &session && display->buffer_acquired);
	if (display->presentation_error) {
		errno = display->presentation_error;
		return false;
	}
	if (display->present_fence < 0)
		return true;
	assert(fake.images[0] && fake.images[1] && fake.gpu.live);
	assert(fake.exported_fd >= 0 && !fake.cpu_access);
	fake.presentation_waits++;
	display->present_fence = -1;
	if (fake.failure == PRESENT_WAIT) {
		display->presentation_error = ETIMEDOUT;
		errno = ETIMEDOUT;
		return false;
	}
	display->shown_page = display->pending_page;
	display->presented = true;
	fake.completed_presentations++;
	return true;
}

int __real_close(int fd);

int __wrap_close(int fd)
{
	assert(fd == fake.exported_fd);
	assert(session.present_fence < 0);
	fake.exported_fd = -1;
	return __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct dma_buf_sync *sync;
	va_list arguments;

	va_start(arguments, request);
	sync = va_arg(arguments, struct dma_buf_sync *);
	va_end(arguments);
	assert(fd == fake.exported_fd && request == DMA_BUF_IOCTL_SYNC);
	assert(fake.fence_complete && !fake.fence_live);
	assert(session.present_fence < 0 && !session.presentation_error);
	if (sync->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW)) {
		assert(!fake.cpu_access);
		fake.sync_starts++;
		if (fake.failure == CPU_START) {
			errno = EACCES;
			return -1;
		}
		fake.cpu_access = true;
	} else {
		assert(sync->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW));
		assert(fake.cpu_access);
		fake.sync_ends++;
		if (fake.failure == CPU_END) {
			errno = EBUSY;
			return -1;
		}
		fake.cpu_access = false;
	}
	return 0;
}

static EGLDisplay current_display(void)
{
	return (EGLDisplay)(uintptr_t)1;
}
static EGLContext current_context(void)
{
	return (EGLContext)(uintptr_t)2;
}

static const char *query_string(EGLDisplay display, EGLint name)
{
	assert(display == current_display() && name == EGL_EXTENSIONS);
	return "EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers";
}

static EGLImageKHR create_image(EGLDisplay display, EGLContext context,
				EGLenum target, EGLClientBuffer buffer,
				const EGLint *attrs)
{
	unsigned int page = fake.image_calls++;
	const EGLint expected[] = {
		EGL_WIDTH,
		6,
		EGL_HEIGHT,
		4,
		EGL_LINUX_DRM_FOURCC_EXT,
		0x36314752,
		EGL_DMA_BUF_PLANE0_FD_EXT,
		fake.exported_fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT,
		page ? 96 : 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT,
		20,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		0,
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
		0,
		EGL_NONE,
	};

	assert(page < 2 && display == current_display());
	assert(context == EGL_NO_CONTEXT && target == EGL_LINUX_DMA_BUF_EXT &&
	       !buffer);
	/* EGL attributes are unordered; the imported storage descriptor is the contract. */
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]) - 1;
	     i += 2) {
		bool found = false;

		for (size_t j = 0; attrs[j] != EGL_NONE; j += 2)
			if (attrs[j] == expected[i]) {
				assert(attrs[j + 1] == expected[i + 1]);
				found = true;
			}
		assert(found);
	}
	if (fake.failure == IMAGE_TWO && page == 1)
		return EGL_NO_IMAGE_KHR;
	fake.images[page] = true;
	return (EGLImageKHR)(uintptr_t)(page + 3);
}

static EGLBoolean destroy_image(EGLDisplay display, EGLImageKHR image)
{
	unsigned int page = (unsigned int)(uintptr_t)image - 3;

	assert(display == current_display() && page < 2 && fake.images[page]);
	assert(session.present_fence < 0);
	fake.images[page] = false;
	return fake.destroy_image_error ? EGL_FALSE : EGL_TRUE;
}

static EGLint egl_error(void)
{
	return EGL_BAD_ALLOC;
}

GLenum glGetError(void)
{
	GLenum error = fake.error;

	fake.error = GL_NO_ERROR;
	return error;
}

void glGetIntegerv(GLenum name, GLint *value)
{
	switch (name) {
	case GL_DRAW_FRAMEBUFFER_BINDING:
		*value = fake.state.framebuffer;
		break;
	case GL_RENDERBUFFER_BINDING:
		*value = fake.state.renderbuffer;
		break;
	case GL_VIEWPORT:
		memcpy(value, fake.state.viewport, sizeof(fake.state.viewport));
		break;
	case GL_CURRENT_PROGRAM:
		*value = fake.state.program;
		break;
	case GL_VERTEX_ARRAY_BINDING:
		*value = fake.state.vertex_array;
		break;
	case GL_ACTIVE_TEXTURE:
		*value = (GLint)fake.state.active_texture;
		break;
	case GL_TEXTURE_BINDING_2D:
		*value = fake.state.textures[fake.state.active_texture -
					     GL_TEXTURE0];
		break;
	case GL_POLYGON_MODE:
		memcpy(value, fake.state.polygon_mode,
		       sizeof(fake.state.polygon_mode));
		break;
	default:
		assert(false);
	}
}

void glGetBooleanv(GLenum name, GLboolean *value)
{
	assert(name == GL_COLOR_WRITEMASK);
	memcpy(value, fake.state.mask, sizeof(fake.state.mask));
}

GLboolean glIsEnabled(GLenum name)
{
	assert(name < 65536);
	assert(fake.gpu.desktop || name != GL_ALPHA_TEST);
	return fake.state.enabled[name];
}
void glEnable(GLenum name)
{
	assert(name < 65536);
	assert(fake.gpu.desktop || name != GL_ALPHA_TEST);
	fake.state.enabled[name] = GL_TRUE;
}
void glDisable(GLenum name)
{
	assert(name < 65536);
	assert(fake.gpu.desktop || name != GL_ALPHA_TEST);
	fake.state.enabled[name] = GL_FALSE;
}
void glUseProgram(GLuint id)
{
	fake.state.program = (GLint)id;
}
void glBindVertexArray(GLuint id)
{
	fake.state.vertex_array = (GLint)id;
}
void glActiveTexture(GLenum unit)
{
	assert(unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + 8);
	fake.state.active_texture = unit;
}
void glBindTexture(GLenum target, GLuint id)
{
	assert(target == GL_TEXTURE_2D);
	fake.state.textures[fake.state.active_texture - GL_TEXTURE0] =
		(GLint)id;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	GLint viewport[] = { x, y, width, height };

	memcpy(fake.state.viewport, viewport, sizeof(viewport));
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue,
		 GLboolean alpha)
{
	GLboolean mask[] = { red, green, blue, alpha };

	memcpy(fake.state.mask, mask, sizeof(mask));
}

static void polygon_mode(GLenum face, GLenum mode)
{
	if (face == GL_FRONT || face == GL_FRONT_AND_BACK)
		fake.state.polygon_mode[0] = (GLint)mode;
	if (face == GL_BACK || face == GL_FRONT_AND_BACK)
		fake.state.polygon_mode[1] = (GLint)mode;
}

#define OBJECT_FUNCTIONS(Name, objects)                       \
	void glGen##Name(GLsizei count, GLuint *ids)          \
	{                                                     \
		for (GLsizei i = 0; i < count; ++i)           \
			ids[i] = allocate(fake.objects);      \
	}                                                     \
	void glDelete##Name(GLsizei count, const GLuint *ids) \
	{                                                     \
		delete_objects(count, ids, fake.objects);     \
	}
OBJECT_FUNCTIONS(VertexArrays, arrays)
OBJECT_FUNCTIONS(Renderbuffers, renderbuffers)
OBJECT_FUNCTIONS(Framebuffers, framebuffers)
#undef OBJECT_FUNCTIONS

void glBindFramebuffer(GLenum target, GLuint id)
{
	assert(target == GL_DRAW_FRAMEBUFFER);
	if (id && (id == internal.page_framebuffer[0] ||
		   id == internal.page_framebuffer[1])) {
		assert(session.present_fence < 0 &&
		       !session.presentation_error);
		fake.imported_binds++;
	}
	fake.state.framebuffer = (GLint)id;
}

void glBindRenderbuffer(GLenum target, GLuint id)
{
	assert(target == GL_RENDERBUFFER);
	fake.state.renderbuffer = (GLint)id;
}

static void image_renderbuffer(GLenum target, GLeglImageOES image)
{
	assert(target == GL_RENDERBUFFER && fake.state.renderbuffer);
	assert((uintptr_t)image >= 3 && (uintptr_t)image <= 4);
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
			       GLenum rb_target, GLuint id)
{
	assert(target == GL_DRAW_FRAMEBUFFER &&
	       attachment == GL_COLOR_ATTACHMENT0);
	assert(rb_target == GL_RENDERBUFFER && fake.renderbuffers[id]);
	internal.page_framebuffer[fake.image_calls - 1] =
		(GLuint)fake.state.framebuffer;
}

void glDrawBuffers(GLsizei count, const GLenum *buffers)
{
	assert(count == 1 && buffers[0] == GL_COLOR_ATTACHMENT0);
}

GLenum glCheckFramebufferStatus(GLenum target)
{
	assert(target == GL_DRAW_FRAMEBUFFER);
	return fake.failure == INCOMPLETE ? GL_FRAMEBUFFER_UNSUPPORTED :
					    GL_FRAMEBUFFER_COMPLETE;
}

GLuint glCreateShader(GLenum type)
{
	assert(type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER);
	return allocate(fake.shaders);
}

void glShaderSource(GLuint id, GLsizei count, const GLchar *const *sources,
		    const GLint *lengths)
{
	assert(fake.shaders[id] && count > 0 && sources && !lengths);
}

void glCompileShader(GLuint id)
{
	assert(fake.shaders[id]);
}
void glGetShaderiv(GLuint id, GLenum name, GLint *value)
{
	assert(fake.shaders[id] && name == GL_COMPILE_STATUS);
	*value = GL_TRUE;
}
void glGetShaderInfoLog(GLuint id, GLsizei size, GLsizei *length, GLchar *log)
{
	(void)id;
	(void)size;
	(void)length;
	log[0] = '\0';
}
void glDeleteShader(GLuint id)
{
	delete_objects(1, &id, fake.shaders);
}
GLuint glCreateProgram(void)
{
	return allocate(fake.programs);
}
void glAttachShader(GLuint program, GLuint shader)
{
	assert(fake.programs[program] && fake.shaders[shader]);
}
void glLinkProgram(GLuint id)
{
	assert(fake.programs[id]);
}
void glGetProgramiv(GLuint id, GLenum name, GLint *value)
{
	assert(fake.programs[id] && name == GL_LINK_STATUS);
	*value = fake.failure != PROGRAM;
}
void glGetProgramInfoLog(GLuint id, GLsizei size, GLsizei *length, GLchar *log)
{
	(void)id;
	(void)size;
	(void)length;
	strcpy(log, "test link failure");
}
void glDeleteProgram(GLuint id)
{
	delete_objects(1, &id, fake.programs);
}
GLint glGetUniformLocation(GLuint id, const GLchar *name)
{
	assert(fake.programs[id] && name);
	return 1;
}
void glUniform1i(GLint location, GLint value)
{
	assert(location == 1);
	(void)value;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	assert(session.present_fence < 0 && !session.presentation_error);
	assert(mode == GL_TRIANGLES && first == 0 && count == 3);
	assert(fake.state.viewport[0] == 0 && fake.state.viewport[1] == 0);
	assert(fake.state.viewport[2] == 6 && fake.state.viewport[3] == 4);
	assert(fake.state.active_texture == GL_TEXTURE0 &&
	       fake.state.textures[0] == 77);
	assert(fake.state.mask[0] && fake.state.mask[1] && fake.state.mask[2] &&
	       fake.state.mask[3]);
	assert(!fake.state.enabled[GL_BLEND] &&
	       !fake.state.enabled[GL_DEPTH_TEST]);
	assert(!fake.state.enabled[GL_SCISSOR_TEST] &&
	       !fake.state.enabled[GL_DITHER]);
	assert(!fake.state.enabled[GL_CULL_FACE] &&
	       !fake.state.enabled[GL_STENCIL_TEST]);
	assert(!fake.state.enabled[GL_RASTERIZER_DISCARD]);
	assert(!fake.state.enabled[GL_SAMPLE_ALPHA_TO_COVERAGE]);
	assert(!fake.state.enabled[GL_SAMPLE_COVERAGE]);
	if (fake.gpu.desktop) {
		assert(!fake.state.enabled[GL_COLOR_LOGIC_OP]);
		assert(!fake.state.enabled[GL_FRAMEBUFFER_SRGB]);
		assert(!fake.state.enabled[GL_ALPHA_TEST]);
		assert(fake.state.polygon_mode[0] == GL_FILL);
		assert(fake.state.polygon_mode[1] == GL_FILL);
	}
	assert(fake.state.framebuffer == (GLint)internal.page_framebuffer[0] ||
	       fake.state.framebuffer == (GLint)internal.page_framebuffer[1]);
	fake.last_draw_page =
		fake.state.framebuffer == (GLint)internal.page_framebuffer[0] ?
			0 :
			1;
	fake.draw_calls++;
	fake.fence_complete = false;
	if (fake.failure == DRAW)
		fake.error = GL_INVALID_OPERATION;
}

GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
	assert(condition == GL_SYNC_GPU_COMMANDS_COMPLETE && flags == 0 &&
	       !fake.fence_live);
	fake.fence_live = true;
	return (GLsync)(uintptr_t)5;
}

void glFlush(void)
{
	if (!fake.fence_live)
		fake.scene_submissions++;
}

GLenum glClientWaitSync(GLsync fence, GLbitfield flags, GLuint64 timeout)
{
	assert((uintptr_t)fence == 5 && fake.fence_live && flags == 0 &&
	       timeout > 0);
	assert(!fake.cpu_access && session.present_fence < 0);
	fake.wait_calls++;
	if (fake.failure == WAIT_ERROR) {
		fake.error = GL_INVALID_OPERATION;
		return GL_WAIT_FAILED;
	}
	if (fake.failure == WAIT_TIMEOUT ||
	    fake.failure == WAIT_TIMEOUT_DELETE_ERROR)
		return GL_TIMEOUT_EXPIRED;
	fake.fence_complete = true;
	return GL_CONDITION_SATISFIED;
}

void glDeleteSync(GLsync fence)
{
	assert((uintptr_t)fence == 5 && fake.fence_live);
	fake.fence_live = false;
	if (fake.failure == WAIT_TIMEOUT_DELETE_ERROR)
		fake.error = GL_INVALID_OPERATION;
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name)
{
	assert(gpu == &fake.gpu && gpu->live);
#define PROC(name_)                \
	if (!strcmp(name, #name_)) \
	return (void *)name_
	PROC(glGetError);
	PROC(glGetIntegerv);
	PROC(glGetBooleanv);
	PROC(glIsEnabled);
	PROC(glEnable);
	PROC(glDisable);
	PROC(glViewport);
	PROC(glColorMask);
	PROC(glActiveTexture);
	PROC(glBindTexture);
	PROC(glBindVertexArray);
	PROC(glGenVertexArrays);
	PROC(glDeleteVertexArrays);
	PROC(glCreateShader);
	PROC(glShaderSource);
	PROC(glCompileShader);
	PROC(glGetShaderiv);
	PROC(glGetShaderInfoLog);
	PROC(glDeleteShader);
	PROC(glCreateProgram);
	PROC(glAttachShader);
	PROC(glLinkProgram);
	PROC(glGetProgramiv);
	PROC(glGetProgramInfoLog);
	PROC(glDeleteProgram);
	PROC(glUseProgram);
	PROC(glGetUniformLocation);
	PROC(glUniform1i);
	PROC(glDrawArrays);
	PROC(glGenRenderbuffers);
	PROC(glBindRenderbuffer);
	PROC(glDeleteRenderbuffers);
	PROC(glGenFramebuffers);
	PROC(glBindFramebuffer);
	PROC(glFramebufferRenderbuffer);
	PROC(glCheckFramebufferStatus);
	PROC(glDeleteFramebuffers);
	PROC(glDrawBuffers);
	PROC(glFenceSync);
	PROC(glClientWaitSync);
	PROC(glDeleteSync);
	PROC(glFlush);
#undef PROC
#define ALIAS(name_, function)    \
	if (!strcmp(name, name_)) \
	return (void *)function
	ALIAS("eglGetCurrentDisplay", current_display);
	ALIAS("eglGetCurrentContext", current_context);
	ALIAS("eglQueryString", query_string);
	ALIAS("eglCreateImageKHR", create_image);
	ALIAS("eglDestroyImageKHR", destroy_image);
	ALIAS("eglGetError", egl_error);
	ALIAS("glEGLImageTargetRenderbufferStorageOES", image_renderbuffer);
	ALIAS("glPolygonMode", polygon_mode);
#undef ALIAS
	assert(false);
	return NULL;
}
