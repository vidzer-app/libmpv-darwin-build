// OSD / subtitle upload check for the gpu-next renderer on macOS OpenGL.
//
// libplacebo uploads OSD and subtitle bitmaps through a PBO when the caller
// asks for an asynchronous upload. Apple's OpenGL drops or shears PBO uploads
// with GL_UNPACK_ROW_LENGTH, and mpv's OSD atlas always has a stride wider
// than the packed area, so text either vanished or showed leftovers of the
// previous line (patches/libplacebo-fix-apple-gl-pbo-row-length.patch).
//
// No media file needed: with force-window, mpv renders the OSD of `show-text`
// on an empty frame, which goes through the same overlay upload as libass
// subtitles. Checks, for each renderer:
// - the text is drawn at all;
// - a short line drawn after a long one is pixel-identical to the same short
//   line drawn by a fresh player (no stale atlas contents).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Mpv/client.h>
#include <Mpv/render_gl.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#define W 1280
#define H 720

static void *get_proc_address(void *ctx, const char *name) {
  (void)ctx;
  static void *gl;
  if (!gl)
    gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
  return dlsym(gl, name);
}

// Shows each text in turn and returns the frame rendered for the last one.
static unsigned char *render_texts(const char *renderer, const char **texts, int count) {
  mpv_handle *mpv = mpv_create();
  mpv_set_option_string(mpv, "vo", "libmpv");
  mpv_set_option_string(mpv, "idle", "yes");
  mpv_set_option_string(mpv, "osd-duration", "100000");
  mpv_set_option_string(mpv, "msg-level", "all=error");
  int r = mpv_initialize(mpv);
  if (r < 0) {
    printf("mpv_initialize: %s\n", mpv_error_string(r));
    return NULL;
  }

  mpv_opengl_init_params gl_init = {.get_proc_address = get_proc_address};
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
      {MPV_RENDER_PARAM_RENDERER, (void *)renderer},
      {0}};
  mpv_render_context *render = NULL;
  r = mpv_render_context_create(&render, mpv, params);
  if (r < 0) {
    printf("mpv_render_context_create(%s): %s\n", renderer, mpv_error_string(r));
    mpv_terminate_destroy(mpv);
    return NULL;
  }

  // vo_libmpv needs the render context, so only ask for the window now.
  mpv_set_property_string(mpv, "force-window", "yes");

  GLuint tex, fbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  mpv_opengl_fbo target = {.fbo = (int)fbo, .w = W, .h = H};
  int flip = 1;
  mpv_render_param render_params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &target}, {MPV_RENDER_PARAM_FLIP_Y, &flip}, {0}};

  for (int i = 0; i < count; i++) {
    const char *cmd[] = {"show-text", texts[i], "100000", NULL};
    mpv_command(mpv, cmd);
    // Let the core pick up the new OSD text, then render a few frames.
    for (int k = 0; k < 10; k++) {
      mpv_wait_event(mpv, 0.02);
      mpv_render_context_update(render);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      mpv_render_context_render(render, render_params);
    }
  }
  glFinish();
  unsigned char *pixels = malloc(W * H * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
  mpv_render_context_free(render);
  mpv_terminate_destroy(mpv);
  return pixels;
}

static int bright_pixels(const unsigned char *p) {
  int n = 0;
  for (int i = 0; i < W * H * 4; i += 4)
    n += p[i] > 200 && p[i + 1] > 200 && p[i + 2] > 200;
  return n;
}

static int differing_pixels(const unsigned char *a, const unsigned char *b) {
  int n = 0;
  for (int i = 0; i < W * H * 4; i += 4)
    n += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 30;
  return n;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  CGLPixelFormatAttribute attributes[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, 0};
  CGLPixelFormatObj pixel_format;
  GLint count;
  CGLContextObj gl;
  CGLChoosePixelFormat(attributes, &pixel_format, &count);
  CGLCreateContext(pixel_format, NULL, &gl);
  CGLSetCurrentContext(gl);

  const char *long_line =
      "We need a better way to measure my intellectual inferiority by comparison.";
  const char *short_line = "Okay.";
  const char *sequence[] = {long_line, short_line};
  const char *fresh[] = {short_line};

  int failures = 0;
  const char *renderers[] = {"gpu-next", "gpu"};
  for (int i = 0; i < 2; i++) {
    const char *renderer = renderers[i];
    unsigned char *first = render_texts(renderer, sequence, 1);
    unsigned char *after = render_texts(renderer, sequence, 2);
    unsigned char *alone = render_texts(renderer, fresh, 1);
    if (!first || !after || !alone) {
      printf("FAIL %s: could not render\n", renderer);
      failures++;
      continue;
    }
    int drawn = bright_pixels(first);
    printf("%s %s: OSD text drawn (%d bright pixels)\n", drawn > 500 ? "ok" : "FAIL", renderer,
           drawn);
    failures += drawn <= 500;
    int stale = differing_pixels(after, alone);
    printf("%s %s: short line after a long one matches a fresh render (%d pixels differ)\n",
           stale == 0 ? "ok" : "FAIL", renderer, stale);
    failures += stale != 0;
    free(first);
    free(after);
    free(alone);
  }
  return failures ? 1 : 0;
}
