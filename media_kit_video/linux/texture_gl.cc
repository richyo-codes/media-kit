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

static EGLSyncKHR create_render_fence(EGLDisplay display) {
  if (!epoxy_has_egl_extension(display, "EGL_KHR_fence_sync") ||
      !epoxy_has_egl_extension(display, "EGL_KHR_wait_sync")) {
    return EGL_NO_SYNC_KHR;
  }
  return eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, NULL);
}

static void wait_for_render_fence(EGLDisplay display, EGLSyncKHR fence) {
  if (fence == EGL_NO_SYNC_KHR) {
    return;
  }

  eglWaitSyncKHR(display, fence, 0);
  eglDestroySyncKHR(display, fence);
}

static void clear_gl_errors(const char* stage) {
  (void)stage;
  while (glGetError() != GL_NO_ERROR) {
  }
}

#if defined(FLUTTER_LINUX_GTK4)
static gboolean media_kit_gtk4_allow_direct_shared_texture(
    VideoOutput* video_output) {
  const gint configured_interop =
      video_output_get_gtk4_texture_interop(video_output);
  if (configured_interop == 1) {
    return FALSE;
  }
  if (configured_interop == 2) {
    return TRUE;
  }

  return TRUE;
}

#endif

#if defined(FLUTTER_LINUX_GTK4)
static gboolean init_egl_image_extensions() {
  static gboolean attempted = FALSE;
  static gboolean available = FALSE;
  if (attempted) {
    return available;
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
  attempted = TRUE;
  available = eglCreateImageKHR != NULL && eglDestroyImageKHR != NULL &&
              glEGLImageTargetTexture2DOES != NULL;
  return available;
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

namespace {

class ScopedVideoOutputRenderLock {
 public:
  explicit ScopedVideoOutputRenderLock(VideoOutput* video_output)
      : video_output_(video_output) {
    video_output_lock_render_state(video_output_);
  }

  ~ScopedVideoOutputRenderLock() {
    video_output_unlock_render_state(video_output_);
  }

 private:
  VideoOutput* video_output_;
};

}  // namespace

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

#if defined(FLUTTER_LINUX_GTK4)
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

static void texture_gl_release_resources_for_rebind(
    TextureGL* self,
    EGLDisplay flutter_display,
    EGLContext flutter_context,
    EGLSurface flutter_draw_surface,
    EGLSurface flutter_read_surface) {
  VideoOutput* video_output = self->video_output;
  const guint32 mpv_texture = self->mpv_texture;

  // Flutter owns the bridge texture in its context. In direct-shared mode the
  // name aliases mpv_texture, so it must only be deleted once below.
  if (self->name != 0 && self->name != mpv_texture) {
    glDeleteTextures(1, &self->name);
  }
  self->name = 0;

  EGLDisplay egl_display = video_output_get_egl_display(video_output);
  EGLContext egl_context = video_output_get_egl_context(video_output);
  EGLSurface egl_surface = video_output_get_egl_surface(video_output);

  if (self->egl_image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(egl_display, self->egl_image);
    self->egl_image = EGL_NO_IMAGE_KHR;
  }

  if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT) {
    EGLSurface draw_read_surface =
        egl_surface != EGL_NO_SURFACE ? egl_surface : EGL_NO_SURFACE;
    if (eglMakeCurrent(egl_display, draw_read_surface, draw_read_surface,
                       egl_context)) {
      if (self->mpv_texture != 0) {
        glDeleteTextures(1, &self->mpv_texture);
      }
      if (self->fbo != 0) {
        glDeleteFramebuffers(1, &self->fbo);
      }
    }
  }

  eglMakeCurrent(flutter_display, flutter_draw_surface, flutter_read_surface,
                 flutter_context);
  self->fbo = 0;
  self->mpv_texture = 0;
  self->use_direct_shared_texture = FALSE;
  self->current_width = 1;
  self->current_height = 1;
}
#endif

static void texture_gl_dispose(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  VideoOutput* video_output = self->video_output;
  
  // Save current context
  EGLDisplay current_display = eglGetCurrentDisplay();
  EGLContext current_context = eglGetCurrentContext();
  EGLSurface current_draw = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface current_read = eglGetCurrentSurface(EGL_READ);
  
  // The bridge texture belongs to Flutter's context. During application
  // shutdown that context may no longer be current (or may already be gone),
  // so never issue GL calls against an unrelated/no context. The driver will
  // reclaim the object when Flutter's context is destroyed.
  if (self->name != 0 && self->name != self->mpv_texture &&
      current_context != EGL_NO_CONTEXT &&
      current_context == self->last_flutter_context) {
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
    EGLSurface egl_surface = video_output_get_egl_surface(video_output);
    
    if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT) {
      EGLSurface draw_read_surface =
          egl_surface != EGL_NO_SURFACE ? egl_surface : EGL_NO_SURFACE;
      if (eglMakeCurrent(egl_display, draw_read_surface, draw_read_surface,
                         egl_context)) {
        if (self->mpv_texture != 0) {
          glDeleteTextures(1, &self->mpv_texture);
          self->mpv_texture = 0;
        }
        if (self->fbo != 0) {
          glDeleteFramebuffers(1, &self->fbo);
          self->fbo = 0;
        }
      }

      // Restore the previous context, or unbind the media context if teardown
      // started without a current Flutter context.
      if (current_display != EGL_NO_DISPLAY &&
          current_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(current_display, current_draw, current_read,
                       current_context);
      } else {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
      }
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
  ScopedVideoOutputRenderLock lock(video_output);
  gint32 required_width = (guint32)video_output_get_width(video_output);
  gint32 required_height = (guint32)video_output_get_height(video_output);

#if defined(FLUTTER_LINUX_GTK4)
  EGLDisplay initial_flutter_display = eglGetCurrentDisplay();
  EGLContext initial_flutter_context = eglGetCurrentContext();
  EGLSurface initial_flutter_draw = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface initial_flutter_read = eglGetCurrentSurface(EGL_READ);
  if (initial_flutter_display != EGL_NO_DISPLAY &&
      initial_flutter_context != EGL_NO_CONTEXT) {
    const gboolean binding_changed = texture_gl_flutter_binding_changed(
        self, initial_flutter_display, initial_flutter_context,
        initial_flutter_draw, initial_flutter_read);
    if (video_output_get_render_context(video_output) == NULL ||
        binding_changed) {
      if (binding_changed &&
          (self->name != 0 || self->fbo != 0 || self->mpv_texture != 0)) {
        texture_gl_release_resources_for_rebind(
            self, initial_flutter_display, initial_flutter_context,
            initial_flutter_draw, initial_flutter_read);
      }
      if (!video_output_rebind_to_flutter_current_context(video_output)) {
        return FALSE;
      }
      texture_gl_record_flutter_binding(
          self, initial_flutter_display, initial_flutter_context,
          initial_flutter_draw, initial_flutter_read);
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
        return FALSE;
      }
      if (video_output_get_render_context(video_output) == NULL ||
          texture_gl_flutter_binding_changed(self, flutter_display,
                                            flutter_context, flutter_draw,
                                            flutter_read)) {
        if (!video_output_rebind_to_flutter_current_context(video_output)) {
          return FALSE;
        }
        if (!init_egl_image_extensions()) {
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
      const guint32 previous_flutter_texture = self->name;
      const guint32 previous_mpv_texture = self->mpv_texture;
#if defined(FLUTTER_LINUX_GTK4)
      can_use_direct_shared_texture =
          media_kit_gtk4_allow_direct_shared_texture(video_output);
      if (!can_use_direct_shared_texture && !init_egl_image_extensions()) {
        g_warning(
            "media_kit: GTK4 EGLImage interop is unavailable; falling back "
            "to direct shared textures.");
        can_use_direct_shared_texture = TRUE;
      }
#endif

      // Switch to mpv's isolated context to create/resize mpv's texture and FBO
      EGLSurface draw_read_surface =
          egl_surface != EGL_NO_SURFACE ? egl_surface : EGL_NO_SURFACE;
      if (!eglMakeCurrent(egl_display, draw_read_surface, draw_read_surface,
                          egl_context)) {
        return FALSE;
      }
      
      // Free previous resources in mpv's context
      if (!first_frame) {
        glDeleteTextures(1, &self->mpv_texture);
        glDeleteFramebuffers(1, &self->fbo);
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
          g_warning(
              "media_kit: Failed to create a GTK4 EGLImage; falling back "
              "to direct shared textures.");
          can_use_direct_shared_texture = TRUE;
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
      } else {
        self->use_direct_shared_texture = FALSE;
        // Switch back to Flutter's context to create/update Flutter's texture.
        if (!eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                            flutter_context)) {
          return FALSE;
        }

        // Free previous Flutter texture.
        if (previous_flutter_texture != 0 &&
            previous_flutter_texture != previous_mpv_texture) {
          glDeleteTextures(1, &previous_flutter_texture);
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
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        clear_gl_errors("populate.egl_image_bridge");
      }
      // Ensure Flutter context is restored after init/resize.
      if (has_flutter_context &&
          !eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                          flutter_context)) {
        return FALSE;
      }
      if (can_use_direct_shared_texture && previous_flutter_texture != 0 &&
          previous_flutter_texture != previous_mpv_texture) {
        glDeleteTextures(1, &previous_flutter_texture);
      }
      
      self->current_width = required_width;
      self->current_height = required_height;
      
      // GTK4 startup can miss the first frame-available wakeup for either
      // interop path, which leaves the texture black until a later invalidate.
      // Keep retrying here for now. The better fix is to find why the initial
      // mark is rejected and move this to a real readiness signal instead of
      // papering over mount timing.
      video_output_notify_texture_update(video_output);
#if defined(FLUTTER_LINUX_GTK4)
      video_output_schedule_initial_frame_wakeups(video_output);
#endif
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
      return FALSE;
    }
    if (video_output_get_render_context(video_output) == NULL ||
        texture_gl_flutter_binding_changed(self, flutter_display,
                                          flutter_context, flutter_draw,
                                          flutter_read)) {
      texture_gl_release_resources_for_rebind(
          self, flutter_display, flutter_context, flutter_draw, flutter_read);
      if (!video_output_rebind_to_flutter_current_context(video_output)) {
        return FALSE;
      }
      texture_gl_record_flutter_binding(self, flutter_display, flutter_context,
                                        flutter_draw, flutter_read);
      video_output_schedule_frame_available(
          video_output, "gtk4_context_rebind", 0);
      return FALSE;
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
      return FALSE;
    }
    
    // Bind mpv's FBO
    glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
    
    // Render mpv frame to mpv's texture
    mpv_opengl_fbo fbo{(gint32)self->fbo, required_width, required_height, 0};
    int flip_y = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    mpv_render_context_render(render_context, params);
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Publish mpv's writes and, when supported, establish explicit ordering
    // before Flutter samples the texture from its shared context.
    EGLSyncKHR render_fence = create_render_fence(egl_display);
    glFlush();
    
    // Restore Flutter's context.
    if (!eglMakeCurrent(flutter_display, flutter_draw, flutter_read,
                        flutter_context)) {
      if (render_fence != EGL_NO_SYNC_KHR) {
        eglDestroySyncKHR(egl_display, render_fence);
      }
      return FALSE;
    }
    wait_for_render_fence(egl_display, render_fence);
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
