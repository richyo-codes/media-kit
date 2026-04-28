// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/texture_gl.h"

#include <epoxy/gl.h>
#include <epoxy/egl.h>

// EGLImage extension function pointers
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

// Define the extension functions
#ifndef eglCreateImageKHR
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
#endif
#ifndef eglDestroyImageKHR
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#endif
#ifndef glEGLImageTargetTexture2DOES
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
#endif

static gboolean has_current_egl_context() {
  return eglGetCurrentDisplay() != EGL_NO_DISPLAY &&
         eglGetCurrentContext() != EGL_NO_CONTEXT;
}

static gboolean init_egl_image_extensions() {
  static gboolean initialized = FALSE;
  if (initialized) {
    return TRUE;
  }

  if (!has_current_egl_context()) {
    return FALSE;
  }

  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  initialized = TRUE;
  return TRUE;
}

static void clear_gl_errors(const char* stage) {
  GLenum err = GL_NO_ERROR;
  gboolean had_error = FALSE;
  while ((err = glGetError()) != GL_NO_ERROR) {
    had_error = TRUE;
    g_printerr("media_kit: TextureGL: GL error 0x%x at %s\n", err, stage);
  }
  (void)had_error;
}

#if defined(FLUTTER_LINUX_GTK4)
static gboolean media_kit_gtk4_allow_direct_shared_texture() {
  const gchar* value = g_getenv("MEDIA_KIT_GTK4_DIRECT_SHARED_TEXTURE");
  if (value == NULL) {
    return TRUE;
  }
  if (g_strcmp0(value, "0") == 0 ||
      g_ascii_strcasecmp(value, "false") == 0 ||
      g_ascii_strcasecmp(value, "no") == 0 ||
      g_ascii_strcasecmp(value, "off") == 0) {
    return FALSE;
  }
  return TRUE;
}
#endif

struct _TextureGL {
  FlTextureGL parent_instance;
  guint32 name;              // Flutter's texture name
  guint32 fbo;               // mpv's FBO
  guint32 mpv_texture;       // mpv's texture
  EGLImageKHR egl_image;     // EGLImage for sharing between contexts
  gboolean use_direct_shared_texture;
  guint32 current_width;
  guint32 current_height;
  EGLDisplay last_flutter_display;
  EGLContext last_flutter_context;
  EGLSurface last_flutter_draw_surface;
  EGLSurface last_flutter_read_surface;
  VideoOutput* video_output;
};

G_DEFINE_TYPE(TextureGL, texture_gl, fl_texture_gl_get_type())

static void texture_gl_init(TextureGL* self) {
  self->name = 0;
  self->fbo = 0;
  self->mpv_texture = 0;
  self->egl_image = EGL_NO_IMAGE_KHR;
  self->use_direct_shared_texture = FALSE;
  self->current_width = 1;
  self->current_height = 1;
  self->last_flutter_display = EGL_NO_DISPLAY;
  self->last_flutter_context = EGL_NO_CONTEXT;
  self->last_flutter_draw_surface = EGL_NO_SURFACE;
  self->last_flutter_read_surface = EGL_NO_SURFACE;
  self->video_output = NULL;
}

static gboolean texture_gl_flutter_binding_changed(TextureGL* self,
                                                   EGLDisplay display,
                                                   EGLContext context,
                                                   EGLSurface draw_surface,
                                                   EGLSurface read_surface) {
  return self->last_flutter_display != display ||
         self->last_flutter_context != context ||
         self->last_flutter_draw_surface != draw_surface ||
         self->last_flutter_read_surface != read_surface;
}

static void texture_gl_record_flutter_binding(TextureGL* self,
                                              EGLDisplay display,
                                              EGLContext context,
                                              EGLSurface draw_surface,
                                              EGLSurface read_surface) {
  self->last_flutter_display = display;
  self->last_flutter_context = context;
  self->last_flutter_draw_surface = draw_surface;
  self->last_flutter_read_surface = read_surface;
}

static void texture_gl_dispose(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  VideoOutput* video_output = self->video_output;
  
  // Save current context
  EGLDisplay current_display = eglGetCurrentDisplay();
  EGLContext current_context = eglGetCurrentContext();
  EGLSurface current_draw = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface current_read = eglGetCurrentSurface(EGL_READ);
  
  // Clean up Flutter's texture (in Flutter's context).
  // If name points to mpv_texture in direct shared mode, delete once in mpv context.
  if (self->name != 0 && self->name != self->mpv_texture) {
    glDeleteTextures(1, &self->name);
    self->name = 0;
  }
  
  // Clean up EGLImage
  if (self->egl_image != EGL_NO_IMAGE_KHR && video_output != NULL) {
    EGLDisplay egl_display = video_output_get_egl_display(video_output);
    eglDestroyImageKHR(egl_display, self->egl_image);
    self->egl_image = EGL_NO_IMAGE_KHR;
  }
  
  // Clean up mpv's OpenGL resources (in mpv's isolated context)
  if (video_output != NULL) {
    EGLDisplay egl_display = video_output_get_egl_display(video_output);
    EGLContext egl_context = video_output_get_egl_context(video_output);
    
    if (egl_context != EGL_NO_CONTEXT) {
      eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);
      
      if (self->mpv_texture != 0) {
        glDeleteTextures(1, &self->mpv_texture);
        self->mpv_texture = 0;
      }
      if (self->fbo != 0) {
        glDeleteFramebuffers(1, &self->fbo);
        self->fbo = 0;
      }
      
      // Restore previous context
      eglMakeCurrent(current_display, current_draw, current_read, current_context);
    }
  }
  
  self->current_width = 1;
  self->current_height = 1;
  self->use_direct_shared_texture = FALSE;
  self->last_flutter_display = EGL_NO_DISPLAY;
  self->last_flutter_context = EGL_NO_CONTEXT;
  self->last_flutter_draw_surface = EGL_NO_SURFACE;
  self->last_flutter_read_surface = EGL_NO_SURFACE;
  self->video_output = NULL;
  G_OBJECT_CLASS(texture_gl_parent_class)->dispose(object);
}

static void texture_gl_class_init(TextureGLClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = texture_gl_populate_texture;
  G_OBJECT_CLASS(klass)->dispose = texture_gl_dispose;
}

TextureGL* texture_gl_new(VideoOutput* video_output) {
  TextureGL* self = TEXTURE_GL(g_object_new(texture_gl_get_type(), NULL));
  self->video_output = video_output;
  return self;
}

gboolean texture_gl_populate_texture(FlTextureGL* texture,
                                     guint32* target,
                                     guint32* name,
                                     guint32* width,
                                     guint32* height,
                                     GError** error) {
  (void)error;
  TextureGL* self = TEXTURE_GL(texture);
  VideoOutput* video_output = self->video_output;
  gint32 required_width = (guint32)video_output_get_width(video_output);
  gint32 required_height = (guint32)video_output_get_height(video_output);

#if defined(FLUTTER_LINUX_GTK4)
  if (video_output_get_render_context(video_output) == NULL) {
    EGLDisplay flutter_display = eglGetCurrentDisplay();
    EGLContext flutter_context = eglGetCurrentContext();
    EGLSurface flutter_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface flutter_read = eglGetCurrentSurface(EGL_READ);

    if (flutter_display != EGL_NO_DISPLAY && flutter_context != EGL_NO_CONTEXT) {
      if (!video_output_rebind_to_flutter_current_context(video_output)) {
        g_printerr(
            "media_kit: TextureGL: Failed to establish GTK4 render context before first frame.\n");
        return FALSE;
      }
      if (!init_egl_image_extensions()) {
        g_printerr(
            "media_kit: TextureGL: EGL extension pointers are unavailable before first frame.\n");
        return FALSE;
      }
      texture_gl_record_flutter_binding(self, flutter_display, flutter_context,
                                        flutter_draw, flutter_read);
    }
  }
#endif

  if (has_current_egl_context()) {
    clear_gl_errors("populate.begin");
  }
  
  if (required_width > 0 && required_height > 0) {
    gboolean first_frame = self->name == 0 || self->fbo == 0 || self->mpv_texture == 0;
    gboolean resize = self->current_width != required_width ||
                      self->current_height != required_height;
    
    if (first_frame || resize) {
      // Save Flutter's current EGL context
      EGLDisplay flutter_display = eglGetCurrentDisplay();
      EGLContext flutter_context = eglGetCurrentContext();
      EGLSurface flutter_draw = eglGetCurrentSurface(EGL_DRAW);
      EGLSurface flutter_read = eglGetCurrentSurface(EGL_READ);
      gboolean has_flutter_context =
          flutter_display != EGL_NO_DISPLAY && flutter_context != EGL_NO_CONTEXT;
#if defined(FLUTTER_LINUX_GTK4)
      if (flutter_display == EGL_NO_DISPLAY ||
          flutter_context == EGL_NO_CONTEXT) {
        g_printerr(
            "media_kit: TextureGL: Flutter EGL context unavailable; deferring frame init.\n");
        return FALSE;
      }
      if (video_output_get_render_context(video_output) == NULL ||
          texture_gl_flutter_binding_changed(self, flutter_display,
                                            flutter_context, flutter_draw,
                                            flutter_read)) {
        if (!video_output_rebind_to_flutter_current_context(video_output)) {
          g_printerr(
              "media_kit: TextureGL: Failed to rebind VideoOutput to Flutter EGL context.\n");
          return FALSE;
        }
        if (!init_egl_image_extensions()) {
          g_printerr(
              "media_kit: TextureGL: EGL extension pointers are unavailable during init/resize.\n");
          return FALSE;
        }
        texture_gl_record_flutter_binding(self, flutter_display, flutter_context,
                                          flutter_draw, flutter_read);
      }
#endif
      EGLDisplay egl_display = video_output_get_egl_display(video_output);
      EGLContext egl_context = video_output_get_egl_context(video_output);
      EGLSurface egl_surface = video_output_get_egl_surface(video_output);
      gboolean can_use_direct_shared_texture = FALSE;
#if defined(FLUTTER_LINUX_GTK4)
      can_use_direct_shared_texture =
          media_kit_gtk4_allow_direct_shared_texture();
#endif

      // Switch to mpv's isolated context to create/resize mpv's texture and FBO
      EGLSurface draw_read_surface =
          egl_surface != EGL_NO_SURFACE ? egl_surface : EGL_NO_SURFACE;
      if (!eglMakeCurrent(egl_display, draw_read_surface, draw_read_surface,
                          egl_context)) {
        g_printerr(
            "media_kit: TextureGL: Failed to make mpv EGL context current for resize. Error: 0x%x\n",
            eglGetError());
        return FALSE;
      }
      
      // Free previous resources in mpv's context
      if (!first_frame) {
        glDeleteTextures(1, &self->mpv_texture);
        glDeleteFramebuffers(1, &self->fbo);
        self->name = 0;
        if (self->egl_image != EGL_NO_IMAGE_KHR) {
          eglDestroyImageKHR(egl_display, self->egl_image);
          self->egl_image = EGL_NO_IMAGE_KHR;
        }
      }
      
      // Create mpv's FBO and texture
      glGenFramebuffers(1, &self->fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
      
      glGenTextures(1, &self->mpv_texture);
      glBindTexture(GL_TEXTURE_2D, self->mpv_texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, required_width, required_height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      
      // Attach mpv's texture to FBO
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, self->mpv_texture, 0);
      
      if (!can_use_direct_shared_texture) {
        // Create EGLImage from mpv's texture for cross-context bridging.
        EGLint egl_image_attribs[] = { EGL_NONE };
        self->egl_image = eglCreateImageKHR(
            egl_display,
            egl_context,
            EGL_GL_TEXTURE_2D_KHR,
            (EGLClientBuffer)(guintptr)self->mpv_texture,
            egl_image_attribs);
        if (self->egl_image == EGL_NO_IMAGE_KHR) {
          g_printerr(
              "media_kit: TextureGL: eglCreateImageKHR failed. Error: 0x%x\n",
              eglGetError());
        }
      } else {
        self->egl_image = EGL_NO_IMAGE_KHR;
      }
      
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
      
      // Flush to ensure mpv's texture is ready
      glFlush();
      if (can_use_direct_shared_texture) {
        self->use_direct_shared_texture = TRUE;
        self->name = self->mpv_texture;
        g_print("media_kit: TextureGL: Using direct shared texture path (GTK4).\n");
      } else {
        self->use_direct_shared_texture = FALSE;
        // Switch back to Flutter's context to create/update Flutter's texture.
        if (!eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                            flutter_context)) {
          g_printerr(
              "media_kit: TextureGL: Failed to restore Flutter EGL context for bridge. Error: 0x%x\n",
              eglGetError());
          return FALSE;
        }

        // Free previous Flutter texture.
        if (!first_frame && self->name != 0 && self->name != self->mpv_texture) {
          glDeleteTextures(1, &self->name);
        }

        // Create Flutter's texture from EGLImage.
        glGenTextures(1, &self->name);
        glBindTexture(GL_TEXTURE_2D, self->name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (glEGLImageTargetTexture2DOES != NULL &&
            self->egl_image != EGL_NO_IMAGE_KHR) {
          glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, self->egl_image);
        } else {
          g_printerr(
              "media_kit: TextureGL: EGLImage bridge extension unavailable.\n");
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        clear_gl_errors("populate.egl_image_bridge");
      }
      // Ensure Flutter context is restored after init/resize.
      if (has_flutter_context &&
          !eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                          flutter_context)) {
        g_printerr(
            "media_kit: TextureGL: Failed to restore Flutter EGL context after init/resize. Error: 0x%x\n",
            eglGetError());
        return FALSE;
      }
#if defined(FLUTTER_LINUX_GTK4)
      if (!self->use_direct_shared_texture) {
        g_print(
            "media_kit: TextureGL: Using EGLImage bridge path (GTK4).\n");
      }
#endif
      
      self->current_width = required_width;
      self->current_height = required_height;
      
      // Notify Flutter about dimension change
      video_output_notify_texture_update(video_output);
    }
    
    // Save Flutter's current context
    EGLDisplay flutter_display = eglGetCurrentDisplay();
    EGLContext flutter_context = eglGetCurrentContext();
    EGLSurface flutter_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface flutter_read = eglGetCurrentSurface(EGL_READ);
    EGLDisplay egl_display = video_output_get_egl_display(video_output);
    EGLContext egl_context = video_output_get_egl_context(video_output);
    EGLSurface egl_surface = video_output_get_egl_surface(video_output);
    mpv_render_context* render_context = video_output_get_render_context(video_output);

#if defined(FLUTTER_LINUX_GTK4)
    if (flutter_display == EGL_NO_DISPLAY ||
        flutter_context == EGL_NO_CONTEXT) {
      g_printerr(
          "media_kit: TextureGL: Flutter EGL context unavailable before render.\n");
      return FALSE;
    }
    if (video_output_get_render_context(video_output) == NULL ||
        texture_gl_flutter_binding_changed(self, flutter_display,
                                          flutter_context, flutter_draw,
                                          flutter_read)) {
      if (!video_output_rebind_to_flutter_current_context(video_output)) {
        g_printerr(
            "media_kit: TextureGL: Failed to rebind VideoOutput before render.\n");
        return FALSE;
      }
      if (!init_egl_image_extensions()) {
        g_printerr(
            "media_kit: TextureGL: EGL extension pointers are unavailable before render.\n");
        return FALSE;
      }
      texture_gl_record_flutter_binding(self, flutter_display, flutter_context,
                                        flutter_draw, flutter_read);
    }
    egl_display = video_output_get_egl_display(video_output);
    egl_context = video_output_get_egl_context(video_output);
    egl_surface = video_output_get_egl_surface(video_output);
    render_context = video_output_get_render_context(video_output);
#endif

    // Switch to mpv's isolated context for rendering
    EGLSurface draw_read_surface =
        egl_surface != EGL_NO_SURFACE ? egl_surface : EGL_NO_SURFACE;
    if (!eglMakeCurrent(egl_display, draw_read_surface, draw_read_surface,
                        egl_context)) {
      g_printerr("media_kit: TextureGL: Failed to make mpv EGL context current for render. Error: 0x%x\n",
                 eglGetError());
      return FALSE;
    }
    
    // Bind mpv's FBO
    glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
    
    // Render mpv frame to mpv's texture
    mpv_opengl_fbo fbo{(gint32)self->fbo, required_width, required_height, 0};
    int flip_y = 0;
    int block_for_target_time = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    mpv_render_context_render(render_context, params);
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Flush to ensure rendering is complete
    glFlush();
    
    // Restore Flutter's context.
    if (!eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                        flutter_context)) {
      g_printerr(
          "media_kit: TextureGL: Failed to restore Flutter EGL context after render. Error: 0x%x\n",
          eglGetError());
      return FALSE;
    }
    clear_gl_errors("populate.after_restore_flutter_context");
  }
  
  *target = GL_TEXTURE_2D;
  *name = self->name;
  *width = self->current_width;
  *height = self->current_height;
  
  if (self->name == 0) {
    // First frame not yet available - create dummy texture in Flutter's context
    glGenTextures(1, &self->name);
    glBindTexture(GL_TEXTURE_2D, self->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    clear_gl_errors("populate.dummy_texture");
    *name = self->name;
    *width = 1;
    *height = 1;
  }
  clear_gl_errors("populate.end");
  
  return TRUE;
}
