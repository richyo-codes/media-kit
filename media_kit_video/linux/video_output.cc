// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/video_output.h"
#include "include/media_kit_video/texture_gl.h"
#include "include/media_kit_video/texture_sw.h"

#include <epoxy/egl.h>
#include <epoxy/glx.h>

#include <cstring>

// Fallback when Flutter toolchain does not export FLUTTER_LINUX_GTK3/GTK4.
#if !defined(FLUTTER_LINUX_GTK4) && !defined(FLUTTER_LINUX_GTK3)
#if GTK_MAJOR_VERSION >= 4
#define FLUTTER_LINUX_GTK4 1
#else
#define FLUTTER_LINUX_GTK3 1
#endif
#endif

#if __has_include(<gdk/wayland/gdkwayland.h>)
#include <gdk/wayland/gdkwayland.h>
#elif __has_include(<gdk/gdkwayland.h>)
#include <gdk/gdkwayland.h>
#endif

#if __has_include(<gdk/x11/gdkx.h>)
#include <gdk/x11/gdkx.h>
#elif __has_include(<gdk/gdkx.h>)
#include <gdk/gdkx.h>
#endif

struct _VideoOutput {
  GObject parent_instance;
  TextureGL* texture_gl;
  EGLDisplay
      egl_display; /* EGL display for mpv rendering (shared with flutter). */
  EGLContext egl_context; /* Isolated EGL context (non-shared). */
  EGLSurface egl_surface; /* Place holder surface for activating egl context */
  guint8* pixel_buffer;
  TextureSW* texture_sw;
  GMutex mutex; /* Only used in S/W rendering. */
  GRecMutex render_mutex;
  mpv_handle* handle;
  mpv_render_context* render_context;
  gint64 width;
  gint64 height;
  VideoOutputConfiguration configuration;
  TextureUpdateCallback texture_update_callback;
  gpointer texture_update_callback_context;
  FlTextureRegistrar* texture_registrar;
  gboolean destroyed;
  gboolean owns_egl_display;
  gboolean owns_egl_surface;
  EGLContext bound_flutter_context;
  gboolean texture_mounted;
  gboolean first_populate_succeeded;
  guint bootstrap_retry_index;
  guint bootstrap_retry_source_id;
  gint64 trace_start_us;
  guint64 trace_sequence;
};

G_DEFINE_TYPE(VideoOutput, video_output, G_TYPE_OBJECT)

namespace {

class ScopedVideoOutputRenderLock {
 public:
  explicit ScopedVideoOutputRenderLock(VideoOutput* self) : self_(self) {
    g_rec_mutex_lock(&self_->render_mutex);
  }

  ~ScopedVideoOutputRenderLock() { g_rec_mutex_unlock(&self_->render_mutex); }

 private:
  VideoOutput* self_;
};

}  // namespace

static gboolean media_kit_video_trace_enabled() {
  static gsize initialized = 0;
  static gboolean enabled = FALSE;
  if (g_once_init_enter(&initialized)) {
    const gchar* value = g_getenv("MEDIA_KIT_VIDEO_TRACE");
    enabled = value != NULL && g_strcmp0(value, "0") != 0 &&
              g_ascii_strcasecmp(value, "false") != 0 &&
              g_ascii_strcasecmp(value, "off") != 0;
    g_once_init_leave(&initialized, 1);
  }
  return enabled;
}

void video_output_trace(VideoOutput* self,
                        const char* event,
                        const char* detail) {
  if (!media_kit_video_trace_enabled()) {
    return;
  }
  const gint64 elapsed_us = g_get_monotonic_time() - self->trace_start_us;
  g_print("media_kit_video_trace seq=%" G_GUINT64_FORMAT
          " elapsed_us=%" G_GINT64_FORMAT
          " self=%p event=%s detail=%s "
          "mounted=%d populated=%d retry=%u size=%" G_GINT64_FORMAT
          "x%" G_GINT64_FORMAT "\n",
          ++self->trace_sequence, elapsed_us, self, event,
          detail != NULL ? detail : "none", self->texture_mounted,
          self->first_populate_succeeded, self->bootstrap_retry_index,
          self->width, self->height);
}

static gboolean video_output_resolve_flutter_egl_config(EGLDisplay display,
                                                        EGLContext context,
                                                        EGLSurface draw_surface,
                                                        EGLSurface read_surface,
                                                        EGLConfig* out_config) {
  g_return_val_if_fail(out_config != NULL, FALSE);

  *out_config = NULL;

  if (display == EGL_NO_DISPLAY) {
    return FALSE;
  }

  EGLint config_id = 0;
  const char* config_source = NULL;

  if (context != EGL_NO_CONTEXT &&
      eglQueryContext(display, context, EGL_CONFIG_ID, &config_id) &&
      config_id > 0) {
    config_source = "context";
  } else if (draw_surface != EGL_NO_SURFACE &&
             eglQuerySurface(display, draw_surface, EGL_CONFIG_ID,
                             &config_id) &&
             config_id > 0) {
    config_source = "draw surface";
  } else if (read_surface != EGL_NO_SURFACE &&
             eglQuerySurface(display, read_surface, EGL_CONFIG_ID,
                             &config_id) &&
             config_id > 0) {
    config_source = "read surface";
  }

  if (config_source == NULL) {
    return FALSE;
  }

  EGLint num_configs = 0;
  EGLint config_attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
  if (eglChooseConfig(display, config_attribs, out_config, 1, &num_configs) &&
      num_configs > 0) {
    return TRUE;
  }

  *out_config = NULL;
  return FALSE;
}

static void video_output_dispose(GObject* object) {
  VideoOutput* self = VIDEO_OUTPUT(object);
  ScopedVideoOutputRenderLock lock(self);
  self->destroyed = TRUE;
  if (self->bootstrap_retry_source_id != 0) {
    g_source_remove(self->bootstrap_retry_source_id);
    self->bootstrap_retry_source_id = 0;
  }

  // Make sure that no more callbacks are invoked from mpv.
  if (self->render_context) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
  }

  // H/W
  if (self->texture_gl) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_gl));

    // Save Flutter's current context before cleanup
    EGLDisplay current_display = eglGetCurrentDisplay();
    EGLContext flutter_context = eglGetCurrentContext();
    EGLSurface flutter_draw_surface = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface flutter_read_surface = eglGetCurrentSurface(EGL_READ);

    // Free mpv and its GL resources while the media context still exists.
    if (self->render_context != NULL) {
      if (self->egl_context != EGL_NO_CONTEXT) {
        EGLSurface current_surface = self->egl_surface != EGL_NO_SURFACE
                                         ? self->egl_surface
                                         : EGL_NO_SURFACE;
        eglMakeCurrent(self->egl_display, current_surface, current_surface,
                       self->egl_context);
      }
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
    }

    g_clear_object(&self->texture_gl);

    // Restore Flutter's context, or unbind the media context before destroying
    // it when disposal started without a current context.
    if (current_display != EGL_NO_DISPLAY &&
        flutter_context != EGL_NO_CONTEXT) {
      eglMakeCurrent(current_display, flutter_draw_surface,
                     flutter_read_surface, flutter_context);
    } else if (self->egl_display != EGL_NO_DISPLAY) {
      eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                     EGL_NO_CONTEXT);
    }

    // Clean up EGL resources
    if (self->egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
    }
    if (self->owns_egl_surface && self->egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(self->egl_display, self->egl_surface);
    }
    self->egl_surface = EGL_NO_SURFACE;
    self->owns_egl_surface = FALSE;
    self->bound_flutter_context = EGL_NO_CONTEXT;
    if (self->owns_egl_display && self->egl_display != EGL_NO_DISPLAY) {
      eglTerminate(self->egl_display);
      self->egl_display = EGL_NO_DISPLAY;
      self->owns_egl_display = FALSE;
    }
  }
  // S/W
  if (self->texture_sw) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_sw));
    g_free(self->pixel_buffer);
    g_object_unref(self->texture_sw);
    if (self->render_context != NULL) {
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
    }
  }

  G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
}

static void video_output_finalize(GObject* object) {
  VideoOutput* self = VIDEO_OUTPUT(object);
  g_rec_mutex_clear(&self->render_mutex);
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(video_output_parent_class)->finalize(object);
}

static void video_output_class_init(VideoOutputClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = video_output_dispose;
  G_OBJECT_CLASS(klass)->finalize = video_output_finalize;
}

static void video_output_init(VideoOutput* self) {
  self->texture_gl = NULL;
  self->egl_display = EGL_NO_DISPLAY;
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
  self->texture_sw = NULL;
  self->pixel_buffer = NULL;
  self->handle = NULL;
  self->render_context = NULL;
  self->width = 0;
  self->height = 0;
  self->configuration = VideoOutputConfiguration{};
  self->texture_update_callback = NULL;
  self->texture_update_callback_context = NULL;
  self->texture_registrar = NULL;
  self->destroyed = FALSE;
  self->owns_egl_display = FALSE;
  self->owns_egl_surface = FALSE;
  self->bound_flutter_context = EGL_NO_CONTEXT;
  self->texture_mounted = FALSE;
  self->first_populate_succeeded = FALSE;
  self->bootstrap_retry_index = 0;
  self->bootstrap_retry_source_id = 0;
  self->trace_start_us = g_get_monotonic_time();
  self->trace_sequence = 0;
  g_mutex_init(&self->mutex);
  g_rec_mutex_init(&self->render_mutex);
}

static gboolean video_output_create_mpv_render_context(VideoOutput* self) {
  mpv_opengl_init_params gl_init_params{
      [](auto, auto name) { return (void*)eglGetProcAddress(name); },
      NULL,
  };

  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, (void*)&gl_init_params},
      {MPV_RENDER_PARAM_INVALID, (void*)0},
      {MPV_RENDER_PARAM_INVALID, (void*)0},
  };

  // VAAPI acceleration requires passing X11/Wayland display.
  GdkDisplay* display = gdk_display_get_default();
#if defined(GDK_WINDOWING_WAYLAND)
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    params[2].type = MPV_RENDER_PARAM_WL_DISPLAY;
    params[2].data = gdk_wayland_display_get_wl_display(display);
  } else
#endif
#if defined(GDK_WINDOWING_X11)
      if (GDK_IS_X11_DISPLAY(display)) {
    params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
#if defined(FLUTTER_LINUX_GTK4)
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#endif
    params[2].data = GDK_DISPLAY_XDISPLAY(display);
#if defined(FLUTTER_LINUX_GTK4)
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
  }
#endif
  {
  }

  if (mpv_render_context_create(&self->render_context, self->handle, params) !=
      0) {
    return FALSE;
  }

  mpv_render_context_set_update_callback(
      self->render_context,
      [](void* data) {
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            [](gpointer data) -> gboolean {
              VideoOutput* self = (VideoOutput*)data;
              ScopedVideoOutputRenderLock lock(self);
              if (self->destroyed || self->texture_gl == NULL) {
                return FALSE;
              }
              video_output_mark_frame_available(self, "mpv_render_update");
              return FALSE;
            },
            g_object_ref(data), g_object_unref);
      },
      self);
  return TRUE;
}

VideoOutput* video_output_new(FlTextureRegistrar* texture_registrar,
                              FlView* view,
                              gint64 handle,
                              VideoOutputConfiguration configuration) {
  VideoOutput* self = VIDEO_OUTPUT(g_object_new(video_output_get_type(), NULL));
  self->texture_registrar = texture_registrar;
  self->handle = (mpv_handle*)handle;
  self->width = configuration.width;
  self->height = configuration.height;
  self->configuration = configuration;
#ifndef MPV_RENDER_API_TYPE_SW
  // MPV_RENDER_API_TYPE_SW must be available for S/W rendering.
  if (!self->configuration.enable_hardware_acceleration) {
  }
  self->configuration.enable_hardware_acceleration = TRUE;
#endif
  mpv_set_option_string(self->handle, "video-sync", "audio");
  // Causes frame drops with `pulse` audio output. (SlotSun/dart_simple_live#42)
  // mpv_set_option_string(self->handle, "video-timing-offset", "0");
  gboolean hardware_acceleration_supported = FALSE;
  if (self->configuration.enable_hardware_acceleration) {
    // Get Flutter's current EGL display (DO NOT share context)
    EGLDisplay flutter_display = eglGetCurrentDisplay();
    EGLContext flutter_context = eglGetCurrentContext();
    EGLSurface flutter_draw_surface = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface flutter_read_surface = eglGetCurrentSurface(EGL_READ);

    bool has_flutter_egl =
        flutter_display != EGL_NO_DISPLAY && flutter_context != EGL_NO_CONTEXT;
    if (has_flutter_egl) {
      self->egl_display = flutter_display;
    }
#if defined(FLUTTER_LINUX_GTK4)
    else {
      self->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (self->egl_display != EGL_NO_DISPLAY) {
        if (eglInitialize(self->egl_display, NULL, NULL)) {
          self->owns_egl_display = TRUE;
        } else {
          self->egl_display = EGL_NO_DISPLAY;
        }
      }
    }
#endif
    if (self->egl_display != EGL_NO_DISPLAY) {
      // Bind OpenGL ES API (Flutter uses OpenGL ES on Linux)
      eglBindAPI(EGL_OPENGL_ES_API);

      // Query Flutter's EGL config and reuse it for compatibility.
      // If unavailable/invalid, fallback to a generic pbuffer-capable config.
      EGLConfig config = NULL;

      if (has_flutter_egl) {
        video_output_resolve_flutter_egl_config(
            self->egl_display, flutter_context, flutter_draw_surface,
            flutter_read_surface, &config);
      }

      if (config == NULL) {
        EGLint num_configs = 0;
        EGLint config_attribs[] = {
            EGL_SURFACE_TYPE,
            EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,
            8,
            EGL_GREEN_SIZE,
            8,
            EGL_BLUE_SIZE,
            8,
            EGL_ALPHA_SIZE,
            8,
            EGL_NONE,
        };
        if (eglChooseConfig(self->egl_display, config_attribs, &config, 1,
                            &num_configs) &&
            num_configs > 0) {
        } else {
        }
      }

      if (config != NULL) {
        // Create an isolated EGL context (NOT shared with Flutter)
        // For GTK4, sharing with Flutter's context improves texture interop.
        EGLint context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION,
            2,
            EGL_NONE,
        };
        EGLContext share_context =
            has_flutter_egl ? flutter_context : EGL_NO_CONTEXT;
        self->egl_context = eglCreateContext(self->egl_display, config,
                                             share_context, context_attribs);

        if (self->egl_context != EGL_NO_CONTEXT) {
          // Use a tiny pbuffer surface for broader EGL compatibility on GTK4
          // drivers.
          EGLint pbuffer_attribs[] = {
              EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
          };
          self->egl_surface = eglCreatePbufferSurface(self->egl_display, config,
                                                      pbuffer_attribs);
          if (self->egl_surface == EGL_NO_SURFACE) {
            self->owns_egl_surface = FALSE;
          } else {
            self->owns_egl_surface = TRUE;
          }

          // Make isolated context current for initialization.
          EGLSurface current_surface = self->egl_surface != EGL_NO_SURFACE
                                           ? self->egl_surface
                                           : EGL_NO_SURFACE;
          if (eglMakeCurrent(self->egl_display, current_surface,
                             current_surface, self->egl_context)) {
            // Create texture with our isolated context
            self->texture_gl = texture_gl_new(self);

            if (fl_texture_registrar_register_texture(
                    texture_registrar, FL_TEXTURE(self->texture_gl))) {
              if (video_output_create_mpv_render_context(self)) {
                hardware_acceleration_supported = TRUE;
              }
            }

            // Restore Flutter's context if available.
            if (has_flutter_egl) {
              eglMakeCurrent(flutter_display, flutter_draw_surface,
                             flutter_read_surface, flutter_context);
            } else {
              eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             EGL_NO_CONTEXT);
            }
          }
        }
      }
    }
  }
#ifdef MPV_RENDER_API_TYPE_SW
  if (!hardware_acceleration_supported) {
    // H/W rendering failed. Fallback to S/W rendering.
    self->pixel_buffer = g_new0(guint8, SW_RENDERING_PIXEL_BUFFER_SIZE);
    self->texture_gl = NULL;
    self->texture_sw = texture_sw_new(self);
    if (fl_texture_registrar_register_texture(texture_registrar,
                                              FL_TEXTURE(self->texture_sw))) {
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, (void*)0},
      };
      if (mpv_render_context_create(&self->render_context, self->handle,
                                    params) == 0) {
        mpv_render_context_set_update_callback(
            self->render_context,
            [](void* data) {
              g_idle_add_full(
                  G_PRIORITY_DEFAULT_IDLE,
                  [](gpointer data) -> gboolean {
                    VideoOutput* self = (VideoOutput*)data;
                    ScopedVideoOutputRenderLock render_lock(self);
                    if (self->destroyed) {
                      return FALSE;
                    }
                    g_mutex_lock(&self->mutex);
                    gint64 width = video_output_get_width(self);
                    gint64 height = video_output_get_height(self);
                    if (width > 0 && height > 0) {
                      gint32 size[]{(gint32)width, (gint32)height};
                      gint32 pitch = 4 * (gint32)width;
                      mpv_render_param params[]{
                          {MPV_RENDER_PARAM_SW_SIZE, size},
                          {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
                          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
                          {MPV_RENDER_PARAM_SW_POINTER, self->pixel_buffer},
                          {MPV_RENDER_PARAM_INVALID, (void*)0},
                      };
                      mpv_render_context_render(self->render_context, params);
                      fl_texture_registrar_mark_texture_frame_available(
                          self->texture_registrar,
                          FL_TEXTURE(self->texture_sw));
                    }
                    g_mutex_unlock(&self->mutex);
                    return FALSE;
                  },
                  g_object_ref(data), g_object_unref);
            },
            self);
      }
    }
  }
#endif
  return self;
}

void video_output_set_texture_update_callback(
    VideoOutput* self,
    TextureUpdateCallback texture_update_callback,
    gpointer texture_update_callback_context) {
  self->texture_update_callback = texture_update_callback;
  self->texture_update_callback_context = texture_update_callback_context;
  // Notify initial dimensions as (1, 1) if |width| & |height| are 0 i.e.
  // texture & video frame size is based on playing file's resolution. This
  // will make sure that `Texture` widget on Flutter's widget tree is actually
  // mounted & |fl_texture_registrar_mark_texture_frame_available| actually
  // invokes the |TextureGL| or |TextureSW| callbacks. Otherwise it will be a
  // never ending deadlock where no video frames are ever rendered.
  gint64 texture_id = video_output_get_texture_id(self);
  if (self->width == 0 || self->height == 0) {
    self->texture_update_callback(texture_id, 1, 1,
                                  self->texture_update_callback_context);
  } else {
    self->texture_update_callback(texture_id, self->width, self->height,
                                  self->texture_update_callback_context);
  }
}

gboolean video_output_rebind_to_flutter_current_context(VideoOutput* self) {
#if !defined(FLUTTER_LINUX_GTK4)
  (void)self;
  return FALSE;
#else
  ScopedVideoOutputRenderLock lock(self);
  EGLDisplay flutter_display = eglGetCurrentDisplay();
  EGLContext flutter_context = eglGetCurrentContext();
  EGLSurface flutter_draw_surface = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface flutter_read_surface = eglGetCurrentSurface(EGL_READ);

  if (flutter_display == EGL_NO_DISPLAY || flutter_context == EGL_NO_CONTEXT) {
    return FALSE;
  }

  if (self->egl_display == flutter_display &&
      self->egl_context != EGL_NO_CONTEXT && self->render_context != NULL &&
      self->bound_flutter_context == flutter_context) {
    return TRUE;
  }

  const gboolean replaced_active_render_context = self->render_context != NULL;
  if (replaced_active_render_context) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
    mpv_render_context_free(self->render_context);
    self->render_context = NULL;
  }
  if (self->egl_context != EGL_NO_CONTEXT &&
      self->egl_display != EGL_NO_DISPLAY) {
    eglDestroyContext(self->egl_display, self->egl_context);
    self->egl_context = EGL_NO_CONTEXT;
  }
  if (self->egl_surface != EGL_NO_SURFACE &&
      self->egl_display != EGL_NO_DISPLAY) {
    if (self->owns_egl_surface) {
      eglDestroySurface(self->egl_display, self->egl_surface);
    }
    self->egl_surface = EGL_NO_SURFACE;
    self->owns_egl_surface = FALSE;
  }
  if (self->owns_egl_display && self->egl_display != EGL_NO_DISPLAY) {
    eglTerminate(self->egl_display);
    self->owns_egl_display = FALSE;
  }
  self->bound_flutter_context = EGL_NO_CONTEXT;

  self->egl_display = flutter_display;

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    return FALSE;
  }

  EGLConfig config = NULL;
  video_output_resolve_flutter_egl_config(self->egl_display, flutter_context,
                                          flutter_draw_surface,
                                          flutter_read_surface, &config);

  if (config == NULL) {
    EGLint num_configs = 0;
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    if (eglChooseConfig(self->egl_display, config_attribs, &config, 1,
                        &num_configs) &&
        num_configs > 0) {
    } else {
      return FALSE;
    }
  }

  EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      2,
      EGL_NONE,
  };
  self->egl_context = eglCreateContext(self->egl_display, config,
                                       flutter_context, context_attribs);
  if (self->egl_context == EGL_NO_CONTEXT) {
    self->bound_flutter_context = EGL_NO_CONTEXT;
    return FALSE;
  }

  EGLint pbuffer_attribs[] = {
      EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
  };
  self->egl_surface =
      eglCreatePbufferSurface(self->egl_display, config, pbuffer_attribs);
  if (self->egl_surface != EGL_NO_SURFACE) {
    self->owns_egl_surface = TRUE;
    if (!eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                        self->egl_context)) {
      eglDestroySurface(self->egl_display, self->egl_surface);
      self->egl_surface = EGL_NO_SURFACE;
      self->owns_egl_surface = FALSE;
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
      self->bound_flutter_context = EGL_NO_CONTEXT;
      return FALSE;
    }
  } else {
    self->owns_egl_surface = FALSE;
    // Prefer surfaceless current context if supported by EGL implementation.
    if (eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       self->egl_context)) {
      self->egl_surface = EGL_NO_SURFACE;
    } else if (flutter_draw_surface != EGL_NO_SURFACE &&
               flutter_read_surface != EGL_NO_SURFACE &&
               eglMakeCurrent(self->egl_display, flutter_draw_surface,
                              flutter_read_surface, self->egl_context)) {
      // Last resort: borrow Flutter's own draw surface for mpv context.
      self->egl_surface = flutter_draw_surface;
    } else {
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
      self->bound_flutter_context = EGL_NO_CONTEXT;
      return FALSE;
    }
  }

  if (!video_output_create_mpv_render_context(self)) {
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (self->owns_egl_surface && self->egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(self->egl_display, self->egl_surface);
    }
    self->egl_surface = EGL_NO_SURFACE;
    self->owns_egl_surface = FALSE;
    eglDestroyContext(self->egl_display, self->egl_context);
    self->egl_context = EGL_NO_CONTEXT;
    self->bound_flutter_context = EGL_NO_CONTEXT;
    eglMakeCurrent(flutter_display, flutter_draw_surface, flutter_read_surface,
                   flutter_context);
    return FALSE;
  }

  self->bound_flutter_context = flutter_context;
  eglMakeCurrent(flutter_display, flutter_draw_surface, flutter_read_surface,
                 flutter_context);

  if (replaced_active_render_context) {
    // libmpv disables video when an active render context is freed. GTK4 must
    // replace the bootstrap context once Flutter's EGL context becomes current,
    // so reinitialize mpv's video output after the replacement. A future native
    // readiness handshake should create the final context before playback and
    // remove the need for this recovery path.
    g_idle_add_full(
        G_PRIORITY_HIGH_IDLE,
        [](gpointer data) -> gboolean {
          VideoOutput* self = VIDEO_OUTPUT(data);
          if (self->destroyed || self->render_context == NULL) {
            return G_SOURCE_REMOVE;
          }
          video_output_trace(self, "mpv_playlist_restart",
                             "gtk4_render_context_replaced");
          const char* command[] = {"playlist-play-index", "current", NULL};
          const int result = mpv_command(self->handle, command);
          if (result < 0) {
            g_warning("media_kit: GTK4 playlist restart failed: %s",
                      mpv_error_string(result));
          }
          return G_SOURCE_REMOVE;
        },
        g_object_ref(self), g_object_unref);
  }
  return TRUE;
#endif
}

void video_output_set_size(VideoOutput* self, gint64 width, gint64 height) {
  ScopedVideoOutputRenderLock lock(self);
  const gboolean size_changed = self->width != width || self->height != height;

  // H/W
  if (self->texture_gl) {
    self->width = width;
    self->height = height;
  }
  // S/W
  if (self->texture_sw) {
    self->width = CLAMP(width, 0, SW_RENDERING_MAX_WIDTH);
    self->height = CLAMP(height, 0, SW_RENDERING_MAX_HEIGHT);
  }

  if (size_changed && width > 0 && height > 0 && self->texture_mounted) {
    video_output_mark_frame_available(self, "video_output_resized");
  }
}

void video_output_lock_render_state(VideoOutput* self) {
  g_rec_mutex_lock(&self->render_mutex);
}

void video_output_unlock_render_state(VideoOutput* self) {
  g_rec_mutex_unlock(&self->render_mutex);
}

void video_output_mark_frame_available(VideoOutput* self, const char* reason) {
  ScopedVideoOutputRenderLock lock(self);
  if (self->destroyed || self->texture_registrar == NULL ||
      self->texture_gl == NULL) {
    return;
  }
  const gboolean marked = fl_texture_registrar_mark_texture_frame_available(
      self->texture_registrar, FL_TEXTURE(self->texture_gl));
  if (media_kit_video_trace_enabled()) {
    g_autofree gchar* detail = g_strdup_printf(
        "reason=%s accepted=%d", reason != NULL ? reason : "none", marked);
    video_output_trace(self, "mark_frame_available", detail);
  }
}

void video_output_mark_texture_mounted(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  if (self->destroyed || self->texture_gl == NULL) {
    return;
  }
  self->texture_mounted = TRUE;
  video_output_trace(self, "flutter_texture_mounted", "platform_channel");
  video_output_mark_frame_available(self, "flutter_texture_mounted");
  video_output_schedule_bootstrap_retry(self, "flutter_texture_mounted");
}

static gboolean video_output_bootstrap_retry_cb(gpointer user_data) {
  VideoOutput* self = VIDEO_OUTPUT(user_data);
  ScopedVideoOutputRenderLock lock(self);
  self->bootstrap_retry_source_id = 0;
  if (!self->destroyed && !self->first_populate_succeeded) {
    video_output_trace(self, "bootstrap_retry_fired", "timer");
    video_output_mark_frame_available(self, "gtk4_bootstrap_retry");
    video_output_schedule_bootstrap_retry(self, "previous_retry_fired");
  }
  return G_SOURCE_REMOVE;
}

void video_output_schedule_bootstrap_retry(VideoOutput* self,
                                           const char* reason) {
#if defined(FLUTTER_LINUX_GTK4)
  static const guint kRetryDelaysMs[] = {16, 50, 250, 1000};
  ScopedVideoOutputRenderLock lock(self);
  if (self->destroyed || self->texture_registrar == NULL ||
      self->texture_gl == NULL || !self->texture_mounted ||
      self->first_populate_succeeded || self->bootstrap_retry_source_id != 0 ||
      self->bootstrap_retry_index >= G_N_ELEMENTS(kRetryDelaysMs)) {
    return;
  }
  const guint delay_ms = kRetryDelaysMs[self->bootstrap_retry_index++];
  if (media_kit_video_trace_enabled()) {
    g_autofree gchar* detail = g_strdup_printf(
        "reason=%s delay_ms=%u", reason != NULL ? reason : "none", delay_ms);
    video_output_trace(self, "bootstrap_retry_scheduled", detail);
  }
  self->bootstrap_retry_source_id = g_timeout_add_full(
      G_PRIORITY_DEFAULT, delay_ms, video_output_bootstrap_retry_cb,
      g_object_ref(self), g_object_unref);
#else
  (void)self;
  (void)reason;
#endif
}

void video_output_mark_populate_succeeded(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  if (self->first_populate_succeeded) {
    return;
  }
  self->first_populate_succeeded = TRUE;
  if (self->bootstrap_retry_source_id != 0) {
    g_source_remove(self->bootstrap_retry_source_id);
    self->bootstrap_retry_source_id = 0;
  }
  video_output_trace(self, "first_populate_succeeded", "mpv_frame_rendered");
}

mpv_render_context* video_output_get_render_context(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->render_context;
}

EGLDisplay video_output_get_egl_display(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->egl_display;
}

EGLContext video_output_get_egl_context(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->egl_context;
}

EGLSurface video_output_get_egl_surface(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->egl_surface;
}

gboolean video_output_is_using_fallback_egl(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->owns_egl_display;
}

gint video_output_get_gtk4_texture_interop(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  return self->configuration.gtk4_texture_interop;
}

gboolean video_output_copy_pixel_buffer(VideoOutput* self,
                                        guint8* destination,
                                        gsize destination_length) {
  g_return_val_if_fail(destination != NULL, FALSE);

  g_mutex_lock(&self->mutex);
  gboolean copied = self->pixel_buffer != NULL &&
                    destination_length <= SW_RENDERING_PIXEL_BUFFER_SIZE;
  if (copied) {
    memcpy(destination, self->pixel_buffer, destination_length);
  }
  g_mutex_unlock(&self->mutex);
  return copied;
}

gint64 video_output_get_width(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  // Fixed width.
  if (self->width) {
    return self->width;
  }

  // Video resolution dependent width.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

gint64 video_output_get_height(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  // Fixed height.
  if (self->width) {
    return self->height;
  }

  // Video resolution dependent height.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}

gint64 video_output_get_texture_id(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  // H/W
  if (self->texture_gl) {
    return (gint64)self->texture_gl;
  }
  // S/W
  if (self->texture_sw) {
    return (gint64)self->texture_sw;
  }
  g_assert_not_reached();
  return -1;
}

void video_output_notify_texture_update(VideoOutput* self) {
  ScopedVideoOutputRenderLock lock(self);
  gint64 id = video_output_get_texture_id(self);
  gint64 width = video_output_get_width(self);
  gint64 height = video_output_get_height(self);
  gpointer context = self->texture_update_callback_context;
  if (self->texture_update_callback != NULL) {
    self->texture_update_callback(id, width, height, context);
  }
}
