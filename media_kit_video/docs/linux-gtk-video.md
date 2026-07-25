# Linux GTK Video Architecture and Hardening

This document tracks the Linux video path shared by the GTK3 and GTK4 Flutter
runners, the GTK4-specific interop work, and the reliability work required
before GTK4 video can be considered production-ready.

## Rendering Path

The preferred GTK4 path is:

```text
mpv FBO
  -> shared EGL texture or EGLImage
  -> Flutter external texture
  -> Flutter compositor framebuffer
  -> GdkGLTexture
  -> GTK snapshot
```

The first interop boundary is owned by `media_kit_video`. The second is owned
by the Flutter Linux embedder. A direct shared texture or EGLImage can avoid a
CPU copy between mpv and Flutter, but it does not by itself guarantee that the
final Flutter framebuffer reaches GTK without readback.

On the current Flutter GTK4 engine path, Wayland uses a shareable EGL texture.
X11/GLX uses full-frame CPU readback. The GTK Cairo renderer is also a software
configuration and is not a performance target.

## Completed Safety Fixes

### Retain video output for queued frame callbacks

Hardware and software mpv update callbacks hold a `VideoOutput` reference
while queued on the GLib main context. Previously a queued callback could read
`destroyed` through an already-freed pointer.

This fix applies to GTK3 and GTK4. Its cost is one atomic GObject reference and
release per queued mpv notification.

### Destroy GL resources with an owning context current

mpv textures and framebuffers are deleted before their EGL context is
destroyed, and Flutter texture GL calls are avoided when Flutter's context is
no longer current. This addresses shutdown failures such as Epoxy asserting
that it cannot find a current GLX or EGL context.

This fix applies to GTK3 and GTK4. The GTK3 shutdown and repeated controller
creation paths still require regression testing because GTK3 and GTK4 reach
Flutter's GL context differently.

### Serialize mutable EGL and mpv render state

A recursive render-state lock protects EGL context rebinding, mpv rendering,
resizing, and disposal. The lock is recursive because texture population calls
helpers that read the same protected state.

This fix applies to GTK3 and GTK4. The lock operation is insignificant compared
with video rendering, but create, resize, context-rebind, and dispose stress
tests must confirm that no lock-order issue remains.

### Centralize GTK4 startup wakeups

One bounded native GTK4 retry sequence replaces duplicate Dart timers and
platform-channel invalidation. GTK3 no longer runs the GTK4 mount workaround.

The retries remain a temporary compatibility measure. An explicit
texture-ready or first-frame-accepted signal is still preferable.

### Snapshot software fallback frames

mpv's software frame is copied into texture-owned upload memory while holding
the producer lock. Flutter uploads the stable snapshot instead of racing mpv as
it writes the next frame.

### Synchronize shared textures with EGL fences

A server-side EGL fence is used when both fence and wait-sync extensions are
available. Flutter waits in its GPU command stream before sampling mpv's
texture, without blocking the CPU. Older implementations retain the `glFlush`
compatibility path.

### Detect the actual GTK presentation API

Flutter engine commit `2789ec1b319` probes GTK's realized GL context and compares
its EGL display with Flutter's. EGL-backed X11 can now use native texture
sharing, while GLX retains CPU readback. Debug output is gated by
`FLUTTER_LINUX_GTK_DEBUG`.

### Release compositor textures safely during resize

Flutter engine commits `30406661324` and `9cd510bee5e` retain the creating EGL
display and the importing GDK GL context. Old compositor textures can therefore
be released during resize without requiring an unrelated context to happen to
be current.

## Remaining Work

Replace the remaining native startup retries with an explicit readiness signal.
Validate fence behavior and resize stability across Mesa, proprietary drivers,
Wayland, X11, and each GSK renderer. Vulkan `VK_SUBOPTIMAL_KHR` reports should
be tracked separately from EGL image failures: they indicate GTK's Vulkan
swapchain no longer exactly matches the resized surface, even though
presentation may continue.

## GTK3 and GTK4 Separation

The current files mix shared mpv lifecycle code with two different graphics
integration strategies. Once the safety changes are validated, split the code
without changing behavior:

```text
video_output.cc       shared lifecycle, sizing, callbacks, software fallback
texture_gl.cc         shared FlTextureGL shell
texture_gl_gtk3.cc    established GTK3 texture integration
texture_gl_gtk4.cc    EGL rebind, direct texture, and EGLImage strategies
egl_context.cc        context save/restore and GL resource ownership
```

Keep compile-time GTK selection at the platform integration boundary. Shared
mpv lifecycle and software rendering should not contain GTK-version branches.

## Validation Matrix

Run create, play, pause, seek, resize, stop, dispose, and repeated window
creation under both GTK variants. Include rapid disposal while frames are
arriving.

For GTK4, cover:

- Wayland with `GSK_RENDERER=ngl` and `GSK_RENDERER=gl`
- Wayland with `GSK_RENDERER=vulkan`
- X11 to exercise engine readback
- `GSK_RENDERER=cairo` as a correctness-only software fallback
- direct shared texture and EGLImage bridge modes
- hardware decode enabled and software rendering forced

Measure frame time, dropped frames, CPU usage, GPU usage, and resident memory.
The test should also fail on GLib/GDK criticals, Epoxy assertions, use-after-free
reports, or callbacks delivered after controller disposal.
