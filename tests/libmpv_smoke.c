// Smoke test for the macOS libmpv build used by Vidzer (media_kit, render API).
//
// Checks the paths that broke when moving to mpv master with cocoa enabled and
// Swift disabled:
// - mpv_create()/mpv_initialize() must not jump to an undefined cocoa_* symbol;
// - an OpenGL render context must be creatable with the gpu-next renderer
//   (libplacebo, Dolby Vision reshaping) and with the legacy gpu renderer;
// - mpv_terminate_destroy() must return (no leftover strong internal client).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include <Mpv/client.h>
#include <Mpv/render_gl.h>
#include <OpenGL/OpenGL.h>

static void *get_proc_address(void *ctx, const char *name) {
  (void)ctx;
  static void *gl;
  if (!gl)
    gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
  return dlsym(gl, name);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int failures = 0;

  mpv_handle *mpv = mpv_create();
  if (!mpv) {
    printf("FAIL mpv_create\n");
    return 1;
  }
  mpv_set_option_string(mpv, "vo", "libmpv");
  mpv_set_option_string(mpv, "msg-level", "all=error");
  int r = mpv_initialize(mpv);
  printf("%s mpv_initialize: %s\n", r < 0 ? "FAIL" : "ok", mpv_error_string(r));
  failures += r < 0;

  CGLPixelFormatAttribute attributes[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, 0};
  CGLPixelFormatObj pixel_format;
  GLint count;
  CGLContextObj gl;
  CGLChoosePixelFormat(attributes, &pixel_format, &count);
  CGLCreateContext(pixel_format, NULL, &gl);
  CGLSetCurrentContext(gl);

  const char *renderers[] = {"gpu-next", "gpu"};
  for (int i = 0; i < 2; i++) {
    mpv_opengl_init_params gl_init = {.get_proc_address = get_proc_address};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_RENDERER, (void *)renderers[i]},
        {0}};
    mpv_render_context *render = NULL;
    r = mpv_render_context_create(&render, mpv, params);
    printf("%s render context %s: %s\n", r < 0 ? "FAIL" : "ok", renderers[i],
           mpv_error_string(r));
    failures += r < 0;
    if (render)
      mpv_render_context_free(render);
  }

  mpv_terminate_destroy(mpv);
  printf("ok mpv_terminate_destroy returned\n");
  return failures ? 1 : 0;
}
