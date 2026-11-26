#pragma once

#include <epoxy/egl.h>
#include <gdk/gdk.h>
#include <mpv/render.h>

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

struct NativeDisplay {
  EGLenum egl_platform;
  mpv_render_param mpv_param;
};

inline NativeDisplay get_native_display(GdkDisplay* display) {
  // GDK's headers select the available backends; the display selects the
  // active one. Other backends have no native display to pass to EGL or mpv.
#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    return {EGL_PLATFORM_WAYLAND_EXT,
            {MPV_RENDER_PARAM_WL_DISPLAY,
             gdk_wayland_display_get_wl_display(display)}};
  }
#endif
#ifdef GDK_WINDOWING_X11
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  if (GDK_IS_X11_DISPLAY(display)) {
    return {EGL_PLATFORM_X11_EXT,
            {MPV_RENDER_PARAM_X11_DISPLAY,
             gdk_x11_display_get_xdisplay(display)}};
  }
  G_GNUC_END_IGNORE_DEPRECATIONS
#endif
  return {0, {MPV_RENDER_PARAM_INVALID, nullptr}};
}
