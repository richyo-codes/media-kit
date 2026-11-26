// Run each display backend in a separate process (see README.md).
#include "../native_display.h"
#include <gtk/gtk.h>

int main(int argc, char** argv) {
  g_assert_cmpint(argc, ==, 2);
  const char* backend = argv[1];
  NativeDisplay none = get_native_display(nullptr);
  g_assert_cmpuint(none.egl_platform, ==, 0);
  g_assert_cmpint(none.mpv_param.type, ==, MPV_RENDER_PARAM_INVALID);
  g_assert_null(none.mpv_param.data);
  if (g_str_equal(backend, "none")) return 0;

  gdk_set_allowed_backends(backend);
  g_assert_true(gtk_init_check());
  auto* display = gdk_display_get_default();
  auto native = get_native_display(display);
#ifdef GDK_WINDOWING_WAYLAND
  if (g_str_equal(backend, "wayland")) {
    g_assert_cmpuint(native.egl_platform, ==, EGL_PLATFORM_WAYLAND_EXT);
    g_assert_cmpint(native.mpv_param.type, ==, MPV_RENDER_PARAM_WL_DISPLAY);
    g_assert_nonnull(native.mpv_param.data);
    g_assert_true(native.mpv_param.data == gdk_wayland_display_get_wl_display(display));
    return 0;
  }
#endif
#ifdef GDK_WINDOWING_X11
  if (g_str_equal(backend, "x11")) {
    g_assert_cmpuint(native.egl_platform, ==, EGL_PLATFORM_X11_EXT);
    g_assert_cmpint(native.mpv_param.type, ==, MPV_RENDER_PARAM_X11_DISPLAY);
    g_assert_nonnull(native.mpv_param.data);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_assert_true(native.mpv_param.data == gdk_x11_display_get_xdisplay(display));
    G_GNUC_END_IGNORE_DEPRECATIONS
    return 0;
  }
#endif
  g_assert_cmpstr(backend, ==, "broadway");
  g_assert_cmpuint(native.egl_platform, ==, 0);
  g_assert_cmpint(native.mpv_param.type, ==, MPV_RENDER_PARAM_INVALID);
  g_assert_null(native.mpv_param.data);
}
