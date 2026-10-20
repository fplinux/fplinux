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
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"

#define GL_COLOR_LOGIC_OP 0x0bf2
#define GL_FRAMEBUFFER_SRGB 0x8db9
#define GL_ALPHA_TEST 0x0bc0
#define GL_POLYGON_MODE 0x0b40
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_LINE 0x1b01
#define GL_POINT 0x1b00
#define GL_FILL 0x1b02

enum failure {
	NONE,
	IMAGE_TWO,
	INCOMPLETE,
	PROGRAM,
	DRAW,
	WAIT_ERROR,
	WAIT_TIMEOUT,
	WAIT_TIMEOUT_DELETE_ERROR,
	CPU_START,
	CPU_END,
	PRESENT,
	PRESENT_WAIT,
};

struct gl_state {
	GLint framebuffer;
	GLint renderbuffer;
	GLint viewport[4];
	GLint program;
	GLint vertex_array;
	GLenum active_texture;
	GLint textures[8];
	GLboolean mask[4];
	GLboolean enabled[65536];
	GLint polygon_mode[2];
};

struct fplinux_gpu {
	bool live;
	bool desktop;
};

static struct {
	struct gl_state state;
	struct fplinux_gpu gpu;
	enum failure failure;
	GLenum error;
	GLuint next_id;
	bool shaders[256];
	bool programs[256];
	bool arrays[256];
	bool framebuffers[256];
	bool renderbuffers[256];
	bool images[2];
	GLuint page_framebuffer[2];
	unsigned int image_calls;
	unsigned int draw_calls;
	unsigned int wait_calls;
	unsigned int sync_starts;
	unsigned int sync_ends;
	unsigned int present_calls;
	unsigned int presentation_waits;
	unsigned int completed_presentations;
	unsigned int scene_submissions;
	unsigned int imported_binds;
	unsigned int releases;
	unsigned int closes;
	unsigned int dispatches;
	unsigned int last_draw_page;
	unsigned int last_present_page;
	int exported_fd;
	bool fence_live;
	bool fence_complete;
	bool cpu_access;
	bool destroy_image_error;
} fake;

static uint8_t mapping[192];
static struct fplinux_drm_session session;

static void reset(enum failure failure)
{
	memset(&fake, 0, sizeof(fake));
	fake.failure = failure;
	fake.next_id = 10;
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

static void caller_state(unsigned int seed)
{
	struct gl_state *state = &fake.state;

	state->framebuffer = 170 + seed;
	state->renderbuffer = 180 + seed;
	state->viewport[0] = 3 + seed;
	state->viewport[1] = 5 + seed;
	state->viewport[2] = 103 + seed;
	state->viewport[3] = 71 + seed;
	state->program = 190 + seed;
	state->vertex_array = 200 + seed;
	state->active_texture = GL_TEXTURE0 + 3;
	state->textures[0] = 210 + seed;
	state->textures[3] = 220 + seed;
	state->mask[0] = GL_FALSE;
	state->mask[1] = GL_TRUE;
	state->mask[2] = GL_FALSE;
	state->mask[3] = GL_TRUE;
	state->enabled[GL_BLEND] = GL_TRUE;
	state->enabled[GL_DEPTH_TEST] = GL_TRUE;
	state->enabled[GL_SCISSOR_TEST] = GL_TRUE;
	state->enabled[GL_DITHER] = GL_TRUE;
	state->enabled[GL_SAMPLE_COVERAGE] = GL_TRUE;
	state->enabled[GL_COLOR_LOGIC_OP] = GL_TRUE;
	state->enabled[GL_FRAMEBUFFER_SRGB] = GL_TRUE;
	state->enabled[GL_ALPHA_TEST] = GL_TRUE;
	state->polygon_mode[0] = GL_LINE;
	state->polygon_mode[1] = GL_POINT;
}

static void assert_state(const struct gl_state *expected)
{
	assert(fake.state.framebuffer == expected->framebuffer);
	assert(fake.state.renderbuffer == expected->renderbuffer);
	assert(fake.state.program == expected->program);
	assert(fake.state.vertex_array == expected->vertex_array);
	assert(fake.state.active_texture == expected->active_texture);
	assert(!memcmp(fake.state.viewport, expected->viewport,
		       sizeof(expected->viewport)));
	assert(!memcmp(fake.state.textures, expected->textures,
		       sizeof(expected->textures)));
	assert(!memcmp(fake.state.mask, expected->mask,
		       sizeof(expected->mask)));
	assert(!memcmp(fake.state.enabled, expected->enabled,
		       sizeof(expected->enabled)));
	assert(!memcmp(fake.state.polygon_mode, expected->polygon_mode,
		       sizeof(expected->polygon_mode)));
}

static GLuint allocate(bool *objects)
{
	GLuint id = fake.next_id++;

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

static void assert_clean(void)
{
	for (unsigned int id = 0; id < 256; ++id) {
		assert(!fake.shaders[id] && !fake.programs[id] &&
		       !fake.arrays[id]);
		assert(!fake.framebuffers[id] && !fake.renderbuffers[id]);
	}
	assert(!fake.images[0] && !fake.images[1]);
	assert(!fake.fence_live && !fake.gpu.live);
	assert(!fake.cpu_access && session.present_fence < 0);
	assert(!session.buffer_acquired);
	assert(fake.exported_fd == -1);
	assert(fake.releases == 1 && fake.closes == 1);
	assert(fake.dispatches == 0);
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
	if (id && (id == fake.page_framebuffer[0] ||
		   id == fake.page_framebuffer[1])) {
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
	fake.page_framebuffer[fake.image_calls - 1] =
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
	assert(fake.state.framebuffer == (GLint)fake.page_framebuffer[0] ||
	       fake.state.framebuffer == (GLint)fake.page_framebuffer[1]);
	fake.last_draw_page =
		fake.state.framebuffer == (GLint)fake.page_framebuffer[0] ? 0 :
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

static struct fplinux_gpu_output *open_output(bool desktop)
{
	struct fplinux_gpu_output *output;
	char error[256];

	output = fplinux_gpu_output_open(
		&session, desktop ? FPLINUX_GPU_OPENGL2 : FPLINUX_GPU_GLES3, 8,
		12, true, error, sizeof(error));
	if (!output)
		fprintf(stderr, "open: %s\n", error);
	assert(output && !error[0]);
	assert(fplinux_gpu_output_gpu(output) == &fake.gpu);
	return output;
}

static void expect_failed_frame(struct fplinux_gpu_output *output,
				int expected_errno)
{
	char first_error[256];
	void *pixels = (void *)(uintptr_t)123;
	size_t stride = 456;
	unsigned int draw_calls = fake.draw_calls;
	unsigned int present_calls = fake.present_calls;

	assert(errno == expected_errno);
	snprintf(first_error, sizeof(first_error), "%s",
		 fplinux_gpu_output_error(output));
	assert(first_error[0]);
	fake.failure = NONE;
	errno = 0;
	assert(!fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
	assert(errno == expected_errno && pixels == (void *)(uintptr_t)123 &&
	       stride == 456);
	assert(!fplinux_gpu_output_present(output) && errno == expected_errno);
	assert(!fplinux_gpu_output_render(output, 77) &&
	       errno == expected_errno);
	assert(!strcmp(fplinux_gpu_output_error(output), first_error));
	assert(fake.draw_calls == draw_calls &&
	       fake.present_calls == present_calls);
}

static void normal_frames(bool desktop)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;
	void *pixels;
	size_t stride;
	const unsigned int selected[] = { 0, 1, 1, 0 };

	reset(NONE);
	caller_state(0);
	expected = fake.state;
	output = open_output(desktop);
	assert_state(&expected);
	assert(fake.image_calls == 2);
	for (unsigned int frame = 0; frame < 4; ++frame) {
		unsigned int shown;

		caller_state(frame + 1);
		expected = fake.state;
		/* A fresh frame must use the current shown page, including external changes. */
		if (frame == 2) {
			assert(fplinux_drm_session_wait_present(&session));
			session.shown_page = 0;
		}
		fake.sync_starts = 0;
		fake.present_calls = 0;
		assert(fplinux_gpu_output_render(output, 77));
		assert_state(&expected);
		assert(fake.last_draw_page == selected[frame] &&
		       fake.fence_complete);
		assert(session.present_fence < 0);
		assert(!fake.sync_starts && !fake.present_calls);
		assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		assert(pixels == mapping + selected[frame] * 96 &&
		       stride == 20);
		assert(fplinux_gpu_output_end_cpu(output));
		shown = session.shown_page;
		assert(fplinux_gpu_output_present(output));
		assert(fake.last_present_page == selected[frame]);
		assert(session.shown_page == shown &&
		       session.pending_page == selected[frame] &&
		       session.present_fence >= 0);
		assert(fake.completed_presentations == frame);
		assert(fake.dispatches == 0);
	}
	assert(fake.wait_calls == 4 && fake.sync_ends == 4);
	assert(fplinux_gpu_output_close(output));
	assert(session.shown_page == 0 && fake.completed_presentations == 4 &&
	       fake.presentation_waits == 4);
	assert_clean();
	/* Closing releases the exclusive borrow and owned descriptor for reopening. */
	fake.image_calls = 0;
	fake.releases = 0;
	fake.closes = 0;
	output = open_output(desktop);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void setup_failure(enum failure failure)
{
	struct gl_state expected;
	char error[256];

	reset(failure);
	caller_state(0);
	expected = fake.state;
	assert(!fplinux_gpu_output_open(&session, FPLINUX_GPU_GLES3, 8, 12,
					true, error, sizeof(error)));
	assert(errno == EIO && error[0]);
	assert_state(&expected);
	assert_clean();
}

static void render_failure(enum failure failure, bool producer_error)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;

	reset(failure);
	output = open_output(false);
	caller_state(3);
	expected = fake.state;
	if (producer_error)
		fake.error = GL_INVALID_OPERATION;
	assert(!fplinux_gpu_output_render(output, 77));
	assert_state(&expected);
	assert(!fake.sync_starts && !fake.present_calls && !fake.fence_live);
	assert(fake.draw_calls == (producer_error ? 0U : 1U));
	expect_failed_frame(
		output, failure == WAIT_TIMEOUT ||
					failure == WAIT_TIMEOUT_DELETE_ERROR ?
				ETIMEDOUT :
				EIO);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void cpu_failure(enum failure failure)
{
	struct fplinux_gpu_output *output;
	void *pixels;
	size_t stride;

	reset(failure);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	if (failure == CPU_START) {
		assert(!fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		expect_failed_frame(output, EACCES);
		assert(fake.sync_starts == 1 && fake.sync_ends == 0);
	} else {
		assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		assert(!fplinux_gpu_output_end_cpu(output));
		expect_failed_frame(output, EBUSY);
		assert(fake.cpu_access && fake.sync_ends == 1);
	}
	assert(fplinux_gpu_output_close(output));
	assert(fake.sync_ends == (failure == CPU_END ? 2U : 0U));
	assert(!fake.cpu_access);
	assert_clean();
}

static void present_failure(void)
{
	struct fplinux_gpu_output *output;
	unsigned int shown;

	reset(PRESENT);
	output = open_output(false);
	shown = session.shown_page;
	assert(fplinux_gpu_output_render(output, 77));
	assert(!fplinux_gpu_output_present(output) && errno == ENODEV);
	assert(session.shown_page == shown && fake.present_calls == 1);
	/* The first failed presentation terminates this output without retrying it. */
	fake.present_calls = 0;
	expect_failed_frame(output, ENODEV);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void close_cpu_access(void)
{
	struct fplinux_gpu_output *output;
	void *pixels;
	size_t stride;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
	assert(fplinux_gpu_output_close(output));
	assert(fake.sync_ends == 1 && !fake.cpu_access && !fake.present_calls);
	assert_clean();
}

static void presentation_wait_failure(void)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;
	unsigned int imported_binds;
	unsigned int scene_submissions;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_present(output));
	assert(session.shown_page == 1 && session.present_fence >= 0);
	caller_state(4);
	expected = fake.state;
	imported_binds = fake.imported_binds;
	scene_submissions = fake.scene_submissions;
	fake.failure = PRESENT_WAIT;
	assert(!fplinux_gpu_output_render(output, 77));
	assert(errno == ETIMEDOUT);
	assert_state(&expected);
	assert(fake.scene_submissions > scene_submissions);
	assert(fake.imported_binds == imported_binds && fake.draw_calls == 1 &&
	       fake.wait_calls == 1 && !fake.sync_starts);
	assert(session.shown_page == 1 && session.present_fence < 0 &&
	       !fake.completed_presentations);
	expect_failed_frame(output, ETIMEDOUT);
	/* A later cleanup error must not replace the deferred presentation error. */
	fake.destroy_image_error = true;
	assert(!fplinux_gpu_output_close(output) && errno == ETIMEDOUT);
	assert_clean();
}

static void close_pending_presentation_failure(void)
{
	struct fplinux_gpu_output *output;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_present(output));
	fake.failure = PRESENT_WAIT;
	fake.destroy_image_error = true;
	assert(!fplinux_gpu_output_close(output) && errno == ETIMEDOUT);
	assert(fake.presentation_waits == 1 && !fake.completed_presentations &&
	       session.shown_page == 1);
	assert_clean();
}

static void reject_unsupported_display(void)
{
	const struct {
		uint32_t format;
		unsigned int source_width;
	} cases[] = {
		{ DRM_FORMAT_NV16, 8 },
		{ DRM_FORMAT_RGB565, 7 },
	};
	char error[256];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		reset(NONE);
		session.format = cases[i].format;
		assert(!fplinux_gpu_output_open(&session, FPLINUX_GPU_GLES3,
						cases[i].source_width, 12, true,
						error, sizeof(error)));
		assert(errno == EINVAL && error[0]);
		assert(!session.buffer_acquired && fake.exported_fd == -1);
		assert(!fake.gpu.live && !fake.image_calls);
		assert(!fake.releases && !fake.closes);
	}
}

int main(void)
{
	normal_frames(false);
	normal_frames(true);
	setup_failure(IMAGE_TWO);
	setup_failure(INCOMPLETE);
	setup_failure(PROGRAM);
	render_failure(NONE, true);
	render_failure(DRAW, false);
	render_failure(WAIT_ERROR, false);
	render_failure(WAIT_TIMEOUT, false);
	render_failure(WAIT_TIMEOUT_DELETE_ERROR, false);
	cpu_failure(CPU_START);
	cpu_failure(CPU_END);
	present_failure();
	close_cpu_access();
	presentation_wait_failure();
	close_pending_presentation_failure();
	reject_unsupported_display();
	puts("gpu-output host contracts: PASS (18 scenarios)");
	return 0;
}
