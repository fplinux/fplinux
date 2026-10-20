/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FPLINUX_QUAKE_GL_H
#define FPLINUX_QUAKE_GL_H

#include <GL/gl.h>
#include <stdbool.h>
#include <stddef.h>

struct fplinux_gpu;

#define FPLINUX_QUAKE_GL_FUNCTIONS(X) \
	X(glAlphaFunc)                \
	X(glBegin)                    \
	X(glBindTexture)              \
	X(glBlendFunc)                \
	X(glClear)                    \
	X(glClearColor)               \
	X(glColor3f)                  \
	X(glColor3fv)                 \
	X(glColor4f)                  \
	X(glColor4fv)                 \
	X(glColor4ubv)                \
	X(glColorMask)                \
	X(glColorPointer)             \
	X(glCopyTexSubImage2D)        \
	X(glCullFace)                 \
	X(glDeleteTextures)           \
	X(glDepthFunc)                \
	X(glDepthMask)                \
	X(glDepthRange)               \
	X(glDisable)                  \
	X(glDisableClientState)       \
	X(glDrawBuffer)               \
	X(glDrawElements)             \
	X(glEnable)                   \
	X(glEnableClientState)        \
	X(glEnd)                      \
	X(glFinish)                   \
	X(glFogf)                     \
	X(glFogfv)                    \
	X(glFogi)                     \
	X(glFrontFace)                \
	X(glGenTextures)              \
	X(glGetError)                 \
	X(glGetFloatv)                \
	X(glGetIntegerv)              \
	X(glGetString)                \
	X(glGetTexParameterfv)        \
	X(glHint)                     \
	X(glLoadIdentity)             \
	X(glLoadMatrixf)              \
	X(glMatrixMode)               \
	X(glNormalPointer)            \
	X(glOrtho)                    \
	X(glPixelStorei)              \
	X(glPolygonMode)              \
	X(glPolygonOffset)            \
	X(glPopMatrix)                \
	X(glPushMatrix)               \
	X(glReadBuffer)               \
	X(glReadPixels)               \
	X(glRotatef)                  \
	X(glScalef)                   \
	X(glShadeModel)               \
	X(glTexCoord2f)               \
	X(glTexCoordPointer)          \
	X(glTexEnvf)                  \
	X(glTexEnvi)                  \
	X(glTexImage2D)               \
	X(glTexParameterf)            \
	X(glTexParameteri)            \
	X(glTexSubImage2D)            \
	X(glTranslatef)               \
	X(glVertex2f)                 \
	X(glVertex3f)                 \
	X(glVertex3fv)                \
	X(glVertexPointer)            \
	X(glViewport)

#define FPLINUX_QUAKE_GL_DECLARE(name) \
	extern __typeof__(&name) fplinux_quake_##name;
FPLINUX_QUAKE_GL_FUNCTIONS(FPLINUX_QUAKE_GL_DECLARE)
#undef FPLINUX_QUAKE_GL_DECLARE

bool fplinux_quake_gl_load(struct fplinux_gpu *gpu, char *error, size_t size);
void fplinux_quake_gl_draw_buffer(GLenum mode);
void fplinux_quake_gl_read_buffer(GLenum mode);

#ifndef FPLINUX_QUAKE_GL_DISPATCH_IMPLEMENTATION
#define glAlphaFunc fplinux_quake_glAlphaFunc
#define glBegin fplinux_quake_glBegin
#define glBindTexture fplinux_quake_glBindTexture
#define glBlendFunc fplinux_quake_glBlendFunc
#define glClear fplinux_quake_glClear
#define glClearColor fplinux_quake_glClearColor
#define glColor3f fplinux_quake_glColor3f
#define glColor3fv fplinux_quake_glColor3fv
#define glColor4f fplinux_quake_glColor4f
#define glColor4fv fplinux_quake_glColor4fv
#define glColor4ubv fplinux_quake_glColor4ubv
#define glColorMask fplinux_quake_glColorMask
#define glColorPointer fplinux_quake_glColorPointer
#define glCopyTexSubImage2D fplinux_quake_glCopyTexSubImage2D
#define glCullFace fplinux_quake_glCullFace
#define glDeleteTextures fplinux_quake_glDeleteTextures
#define glDepthFunc fplinux_quake_glDepthFunc
#define glDepthMask fplinux_quake_glDepthMask
#define glDepthRange fplinux_quake_glDepthRange
#define glDisable fplinux_quake_glDisable
#define glDisableClientState fplinux_quake_glDisableClientState
#define glDrawBuffer fplinux_quake_gl_draw_buffer
#define glDrawElements fplinux_quake_glDrawElements
#define glEnable fplinux_quake_glEnable
#define glEnableClientState fplinux_quake_glEnableClientState
#define glEnd fplinux_quake_glEnd
#define glFinish fplinux_quake_glFinish
#define glFogf fplinux_quake_glFogf
#define glFogfv fplinux_quake_glFogfv
#define glFogi fplinux_quake_glFogi
#define glFrontFace fplinux_quake_glFrontFace
#define glGenTextures fplinux_quake_glGenTextures
#define glGetError fplinux_quake_glGetError
#define glGetFloatv fplinux_quake_glGetFloatv
#define glGetIntegerv fplinux_quake_glGetIntegerv
#define glGetString fplinux_quake_glGetString
#define glGetTexParameterfv fplinux_quake_glGetTexParameterfv
#define glHint fplinux_quake_glHint
#define glLoadIdentity fplinux_quake_glLoadIdentity
#define glLoadMatrixf fplinux_quake_glLoadMatrixf
#define glMatrixMode fplinux_quake_glMatrixMode
#define glNormalPointer fplinux_quake_glNormalPointer
#define glOrtho fplinux_quake_glOrtho
#define glPixelStorei fplinux_quake_glPixelStorei
#define glPolygonMode fplinux_quake_glPolygonMode
#define glPolygonOffset fplinux_quake_glPolygonOffset
#define glPopMatrix fplinux_quake_glPopMatrix
#define glPushMatrix fplinux_quake_glPushMatrix
#define glReadBuffer fplinux_quake_gl_read_buffer
#define glReadPixels fplinux_quake_glReadPixels
#define glRotatef fplinux_quake_glRotatef
#define glScalef fplinux_quake_glScalef
#define glShadeModel fplinux_quake_glShadeModel
#define glTexCoord2f fplinux_quake_glTexCoord2f
#define glTexCoordPointer fplinux_quake_glTexCoordPointer
#define glTexEnvf fplinux_quake_glTexEnvf
#define glTexEnvi fplinux_quake_glTexEnvi
#define glTexImage2D fplinux_quake_glTexImage2D
#define glTexParameterf fplinux_quake_glTexParameterf
#define glTexParameteri fplinux_quake_glTexParameteri
#define glTexSubImage2D fplinux_quake_glTexSubImage2D
#define glTranslatef fplinux_quake_glTranslatef
#define glVertex2f fplinux_quake_glVertex2f
#define glVertex3f fplinux_quake_glVertex3f
#define glVertex3fv fplinux_quake_glVertex3fv
#define glVertexPointer fplinux_quake_glVertexPointer
#define glViewport fplinux_quake_glViewport
#endif

#endif
