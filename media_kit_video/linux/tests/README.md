# Native display selection (GTK4)

From `media_kit_video/linux`, build the standalone test with:

```sh
c++ -std=c++17 tests/native_display_test.cc $(pkg-config --cflags --libs gtk4 epoxy mpv) -o /tmp/native-display-test
/tmp/native-display-test none
xvfb-run -a /tmp/native-display-test x11
```

For Wayland, run `/tmp/native-display-test wayland` in a Wayland session
or set `WAYLAND_DISPLAY` to a private headless Weston's socket.
For a real unsupported backend, start `gtk4-broadwayd :17` and run
`BROADWAY_DISPLAY=:17 /tmp/native-display-test broadway`. Each invocation
requires the specified GDK backend; a different backend cannot silently pass.
No Flutter engine, mpv playback, or EGL context is needed.
