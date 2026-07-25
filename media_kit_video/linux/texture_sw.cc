// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/texture_sw.h"

struct _TextureSW {
  FlPixelBufferTexture parent_instance;
  guint32 current_width;
  guint32 current_height;
  guint8* upload_buffer;
  gsize upload_buffer_length;
  VideoOutput* video_output;
};

G_DEFINE_TYPE(TextureSW, texture_sw, fl_pixel_buffer_texture_get_type())

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

static void texture_sw_init(TextureSW* self) {
  self->current_width = 1;
  self->current_height = 1;
  self->upload_buffer = NULL;
  self->upload_buffer_length = 0;
  self->video_output = NULL;
}

static void texture_sw_dispose(GObject* object) {
  TextureSW* self = TEXTURE_SW(object);
  g_clear_pointer(&self->upload_buffer, g_free);
  self->upload_buffer_length = 0;
  G_OBJECT_CLASS(texture_sw_parent_class)->dispose(object);
}

static void texture_sw_class_init(TextureSWClass* klass) {
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels = texture_sw_copy_pixels;
  G_OBJECT_CLASS(klass)->dispose = texture_sw_dispose;
}

TextureSW* texture_sw_new(VideoOutput* video_output) {
  TextureSW* self = TEXTURE_SW(g_object_new(texture_sw_get_type(), NULL));
  self->video_output = video_output;
  return self;
}

gboolean texture_sw_copy_pixels(FlPixelBufferTexture* texture,
                                const guint8** buffer,
                                guint32* width,
                                guint32* height,
                                GError** error) {
  TextureSW* self = TEXTURE_SW(texture);
  VideoOutput* video_output = self->video_output;
  ScopedVideoOutputRenderLock lock(video_output);
  gint32 required_width = (guint32)video_output_get_width(video_output);
  gint32 required_height = (guint32)video_output_get_height(video_output);
  if (required_width > 0 && required_height > 0) {
    const gsize required_length =
        (gsize)required_width * (gsize)required_height * 4;
    if (self->upload_buffer_length < required_length) {
      guint8* upload_buffer =
          (guint8*)g_try_realloc(self->upload_buffer, required_length);
      if (upload_buffer == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                            "Failed to allocate software video upload buffer");
        return FALSE;
      }
      self->upload_buffer = upload_buffer;
      self->upload_buffer_length = required_length;
    }
    if (!video_output_copy_pixel_buffer(video_output, self->upload_buffer,
                                        required_length)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Failed to copy software video frame");
      return FALSE;
    }
    if (self->current_width != required_width ||
        self->current_height != required_height) {
      self->current_width = required_width;
      self->current_height = required_height;
      // Notify Flutter about the change in texture's dimensions.
      video_output_notify_texture_update(video_output);
    }
    *buffer = self->upload_buffer;
    *width = required_width;
    *height = required_height;
  }
  return TRUE;
}
