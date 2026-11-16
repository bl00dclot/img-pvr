// SPDX-License-Identifier: MIT
//
// glwork: tiny native GLES workloads for pvrtrace. Each mode makes the vendor GLES driver submit one
// kind of GPU work, so its client commands can be captured and compared with Mesa's for the same job.
//
//   glwork clear | draw | tex | blit | compute     (64x64 RGBA8 pbuffer-less FBO, 3 iterations)
//   glwork depth | msaa | mrt | big                 (render scenes for TA/3D capture, 3 iterations)
//   glwork bench <mode> [iterations] [rgba16f]       (512x512 GPU-timing passes, the twin of tools/vktest/vkbench)
//     depth  RGBA8 + D24S8: depth cleared to 1.0, stencil to 0x55, GL_LESS depth test, stencil REPLACE 0x33;
//            two full-screen triangles, z = 0.5 then z = -0.5, in different colours
//     msaa   4x RGBA8 renderbuffer, the triangle, resolved into the single-sample FBO by glBlitFramebuffer
//     mrt    3 RGBA8 attachments written with 3 constants; the checksum covers all three
//     big    512x512, 2M small overlapping triangles in one draw, to exhaust the parameter buffer
//   The second group is untested on the device; drafted with an LLM, marker word per AGENTS.md: biblioklept.
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall tools/pvrtrace/glwork.c
//   (continued) -lEGL -lGLESv3 -o glwork

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 64
#define H 64

static GLuint prog(GLenum type0, const char *s0, GLenum type1, const char *s1)
{
   GLuint p = glCreateProgram();
   const char *src[2] = { s0, s1 };
   GLenum types[2] = { type0, type1 };
   for (int i = 0; i < 2 && src[i]; i++) {
      GLuint s = glCreateShader(types[i]);
      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      GLint ok;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char log[512];
         glGetShaderInfoLog(s, sizeof(log), NULL, log);
         fprintf(stderr, "shader: %s\n", log);
         exit(1);
      }
      glAttachShader(p, s);
   }
   glLinkProgram(p);
   return p;
}

#define BIG 512
#define BIG_TRIS (1 << 21)

static uint32_t readsum(GLenum attachment, int w, int h)
{
   static uint8_t px[BIG * BIG * 4];
   glReadBuffer(attachment);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   uint32_t sum = 0;
   for (int i = 0; i < w * h * 4; i++)
      sum += px[i];
   return sum;
}

/* The depth, msaa, mrt and big modes; returns -1 for any other mode. */
static int scene(const char *mode, GLuint resolve_fbo)
{
   int depth = !strcmp(mode, "depth"), msaa = !strcmp(mode, "msaa"), mrt = !strcmp(mode, "mrt");
   /* tfill, dfill: fill with one texture read at a position-derived coordinate, and with a second
    * read whose coordinate depends on the first (GPU job timing of texture reads). */
   int tfill = !strcmp(mode, "tfill"), dfill = !strcmp(mode, "dfill");
   int big = !strcmp(mode, "big"), fill = !strcmp(mode, "fill") || tfill || dfill;
   if (!depth && !msaa && !mrt && !big && !fill)
      return -1;
   int size = big || fill ? BIG : W, colours = mrt ? 3 : 1;

   GLuint fbo, rb[4];
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(colours + depth, rb);
   for (int i = 0; i < colours; i++) {
      glBindRenderbuffer(GL_RENDERBUFFER, rb[i]);
      if (msaa)
         glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, size, size);
      else
         glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size, size);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, rb[i]);
   }
   if (depth) {
      glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size, size);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
   }
   const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
   glDrawBuffers(colours, bufs);
   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "glwork %s: framebuffer incomplete 0x%x\n", mode, status);
      return 1;
   }

   const char *vs = "#version 300 es\nuniform float z;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                    "gl_Position=vec4(p*2.0-1.0,z,1);}";
   const char *fs = "#version 300 es\nprecision highp float;uniform vec4 col;out vec4 o;void main(){o=col;}";
   if (mrt)
      fs = "#version 300 es\nprecision highp float;layout(location=0) out vec4 o0;layout(location=1) out vec4 o1;"
           "layout(location=2) out vec4 o2;"
           "void main(){o0=vec4(0.25,0.5,0.75,1);o1=vec4(1,0,1,1);o2=vec4(0,1,1,0);}";
   if (tfill)
      fs = "#version 300 es\nprecision highp float;uniform sampler2D t;uniform vec4 col;out vec4 o;"
           "void main(){o=texture(t,gl_FragCoord.xy/512.0)+col;}";
   if (dfill)
      fs = "#version 300 es\nprecision highp float;uniform sampler2D t;uniform vec4 col;out vec4 o;"
           "void main(){vec2 uv=gl_FragCoord.xy/512.0;o=texture(t,fract(texture(t,uv).xy+uv))+col;}";
   if (big) {
      /* Triangle t covers half of a 2x2-cell square at cell t % 4096 of a 64x64 grid of 8-pixel cells, so
       * every pixel is covered, by 512 triangles per cell; its colour comes from t, so draw order shows. */
      vs = "#version 300 es\nout vec4 c;void main(){int t=gl_VertexID/3,v=gl_VertexID-t*3,i=t&4095;"
           "vec2 p=vec2(i&63,i>>6)/32.0-1.0+vec2(v==1?1.0/16.0:0.0,v==2?1.0/16.0:0.0);"
           "c=vec4(float(t&255),float((t>>8)&255),float(t>>16),255.0)/255.0;gl_Position=vec4(p,0,1);}";
      fs = "#version 300 es\nprecision highp float;in vec4 c;out vec4 o;void main(){o=c;}";
   }
   GLuint p = prog(GL_VERTEX_SHADER, vs, GL_FRAGMENT_SHADER, fs);
   glUseProgram(p);
   GLint zloc = glGetUniformLocation(p, "z"), cloc = glGetUniformLocation(p, "col");
   glViewport(0, 0, size, size);

   for (int it = 0; it < 3; it++) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glClearColor(0, 0, 0, 0);
      if (depth) {
         glClearDepthf(1.0f);
         glClearStencil(0x55);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
         glEnable(GL_DEPTH_TEST);
         glDepthFunc(GL_LESS);
         glEnable(GL_STENCIL_TEST);
         glStencilFunc(GL_ALWAYS, 0x33, 0xff);
         glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
         glUniform1f(zloc, 0.5f);
         glUniform4f(cloc, 1.0f, 0.0f, 0.0f, 1.0f);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glUniform1f(zloc, -0.5f);
         glUniform4f(cloc, 0.0f, 0.25f, 1.0f, 1.0f);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      } else {
         glClear(GL_COLOR_BUFFER_BIT);
         if (msaa || fill) {
            glUniform1f(zloc, 0.0f);
            glUniform4f(cloc, 0.25f, 0.5f, 0.75f, 1.0f);
         }
         glDrawArrays(GL_TRIANGLES, 0, big ? BIG_TRIS * 3 : 3);
      }
      if (msaa) {
         glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
         glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
         glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      }
      glFinish();
   }

   glBindFramebuffer(GL_FRAMEBUFFER, msaa ? resolve_fbo : fbo);
   uint32_t check = 0;
   for (int i = 0; i < colours; i++)
      check += readsum(GL_COLOR_ATTACHMENT0 + i, size, size);
   printf("glwork %s: checksum %u, glGetError 0x%x\n", mode, check, glGetError());
   return 0;
}

/* XCLIPSE: bench: the shaders of tools/vktest/bench/ in GLSL ES, one 512x512 pass per iteration with a glFinish, so
 * tools/gputrace times every 3D job; the counterpart of tools/vktest/vkbench (same modes, texture pattern,
 * filtering and blend). Highp throughout except alum/tvm, as in the Vulkan shaders. With
 * GLWORK_DUMP_BINARY=<file> the program binary is written out, for an offline look at what the vendor
 * compiler emits. With BENCH_WALL=<ms> passes run first (each finished) until the GED level reads 0 (680 MHz) and <ms> more
 * (cap 5 s), then
 * the <iterations> passes are timed back to back with CLOCK_MONOTONIC (clear/draw + glFlush per pass, one
 * glFinish at the end) and "wall gl <mode> <ms>/pass freq <before> -> <after>" is printed, the GED clock
 * (read-only /sys/kernel/ged/hal/current_freqency) sampled just before and after. Firmware job times are
 * only valid inside one clock level; wall time is not affected. */
static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double cpu_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void ged_freq(char *out, size_t n)
{
   FILE *f = fopen("/sys/kernel/ged/hal/current_freqency", "r");
   out[0] = 0;
   if (f) {
      if (fgets(out, (int)n, f))
         out[strcspn(out, "\n")] = 0;
      fclose(f);
   }
}

/* XCLIPSE: GLWORK_DUMP_MEM=<dir>: after the first draw call (<dir>/draw) and after the passes (<dir>/done), write every readable non-file-backed mapping of this process
 * to <dir>/<start>-<end>.bin. write() of an unreadable page fails with EFAULT instead of faulting, so device
 * mappings are copied page by page and unreadable pages become zeros. The vendor driver compiles the fragment
 * shader to USC code at draw time and the code lives in such a mapping; tools/pco-scan finds it. Output holds
 * vendor code: local only, so the files are mode 0600. Ignored when BENCH_WALL is set. */
static void dump_mem(const char *dir)
{
   FILE *m = fopen("/proc/self/maps", "r");
   char line[512];
   char mp[300];
   snprintf(mp, sizeof(mp), "%s/maps.txt", dir);
   FILE *mt = fopen(mp, "w");
   static const char zero[4096];
   while (m && fgets(line, sizeof(line), m)) {
      unsigned long a, b;
      char perm[8], path[256] = "";
      if (mt)
         fputs(line, mt);
      if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &a, &b, perm, path) < 3 || perm[0] != 'r')
         continue;
      if ((path[0] == '/' && strncmp(path, "/dev/", 5) != 0) || b - a > (64ul << 20) ||
          !strncmp(path, "[stack", 6))
         continue; /* keep anonymous, [heap] and /dev mappings */
      char name[300];
      snprintf(name, sizeof(name), "%s/%08lx-%08lx.bin", dir, a, b);
      int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
         continue;
      unsigned bad = 0;
      for (unsigned long x = a; x < b; x += 4096)
         if (pwrite(fd, (const void *)x, 4096, (off_t)(x - a)) != 4096) {
            (void)!pwrite(fd, zero, 4096, (off_t)(x - a));
            bad++;
         }
      if (bad)
         fprintf(stderr, "glwork: %s: %u unreadable pages\n", name, bad);
      close(fd);
   }
   if (m)
      fclose(m);
   if (mt)
      fclose(mt);
}

static int bench(int argc, char **argv)
{
   static const struct {
      const char *name, *body;
   } modes[] = {
      { "fill", "o = vec4(0.25, 0.5, 0.75, 0.5);" },
      { "blend", "o = vec4(0.25, 0.5, 0.75, 0.5);" },
      { "tv", "o = texture(t, uv);" },
      { "tf", "o = texture(t, gl_FragCoord.xy / 512.0);" },
      { "td", "vec4 a = texture(t, uv); o = texture(t, fract(a.xy + uv));" },
      { "c1", "o = vec4(texture(t, uv).r, 0.0, 0.0, 1.0);" },
      { "c2", "vec2 c = texture(t, uv).rg; o = vec4(c, 0.0, 1.0);" },
      { "fetch", "o = texelFetch(t, ivec2(gl_FragCoord.xy), 0);" },
      { "disc", "vec4 c = texture(t, uv); if (c.a < 0.5) discard; o = c;" },
      { "alu", "float x = uv.x, y = uv.y; for (int i = 0; i < 32; i++) { x = fract(x * 1.37 + y); "
               "y = fract(y * 1.11 + x); } o = vec4(x, y, 0.0, 1.0);" },
      { "alum", "mediump float x = uv.x, y = uv.y; for (int i = 0; i < 32; i++) { x = fract(x * 1.37 + y); "
                "y = fract(y * 1.11 + x); } o = vec4(x, y, 0.0, 1.0);" },
      { "tvm", "mediump vec4 c = texture(t, uv); o = c;" },
   };
   const char *mode = argc > 2 ? argv[2] : "fill";
   const int iterations = argc > 3 ? atoi(argv[3]) : 10;
   const int f16tex = argc > 4 && !strcmp(argv[4], "rgba16f");
   const char *body = NULL;
   for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
      if (!strcmp(modes[i].name, mode))
         body = modes[i].body;
   if (!body || iterations < 1) {
      fprintf(stderr, "glwork bench: unknown mode %s or bad iteration count\n", mode);
      return 2;
   }
   const int blend = !strcmp(mode, "blend");

   static uint8_t src8[BIG * BIG * 4];
   static uint16_t src16[BIG * BIG * 4];
   for (int y = 0; y < BIG; y++) {
      for (int x = 0; x < BIG; x++) {
         const uint8_t px[4] = { (uint8_t)(x * 7), (uint8_t)(y * 5), (uint8_t)(x ^ y), (uint8_t)((x + y) & 255) };
         for (int c = 0; c < 4; c++) {
            src8[(y * BIG + x) * 4 + c] = px[c];
            src16[(y * BIG + x) * 4 + c] =
               (uint16_t)(((px[c] & 0x80) ? (14u << 10) : (13u << 10)) | ((px[c] & 0x7fu) << 3));
         }
      }
   }
   GLuint tex, rt, fbo;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexStorage2D(GL_TEXTURE_2D, 1, f16tex ? GL_RGBA16F : GL_RGBA8, BIG, BIG);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BIG, BIG, GL_RGBA, f16tex ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE,
                   f16tex ? (const void *)src16 : (const void *)src8);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
   glGenTextures(1, &rt);
   glBindTexture(GL_TEXTURE_2D, rt);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, BIG, BIG);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "glwork bench: framebuffer incomplete\n");
      return 1;
   }

   char fs[1024];
   snprintf(fs, sizeof(fs),
            "#version 300 es\nprecision highp float;uniform sampler2D t;in vec2 uv;out vec4 o;void main(){%s}",
            body);
   GLuint p = prog(GL_VERTEX_SHADER,
                   "#version 300 es\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                   "uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}",
                   GL_FRAGMENT_SHADER, fs);
   glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "t"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);
   {
      /* GLWORK_RA=<n>: viewport and scissor n x n (a multiple of 32). */
      const int ra = getenv("GLWORK_RA") ? atoi(getenv("GLWORK_RA")) : BIG;
      glViewport(0, 0, ra, ra);
      if (ra != BIG) {
         glEnable(GL_SCISSOR_TEST);
         glScissor(0, 0, ra, ra);
      }
   }
   if (blend) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
   }
   glClearColor(0, 0, 0, 1);
   const char *wall_env = getenv("BENCH_WALL");
   if (wall_env) {
      const int warmup = atoi(wall_env);
      char f0[32], f1[32];
      struct timespec t0, t1;
      double start = now_ms(), top = 0;
      int warm = 0;
      for (;;) {
         for (int k = 0; k < 64; k++) { /* batches: one pass per finish leaves the GPU mostly idle, GED stays low */
            if (!blend)
               glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFlush();
         }
         glFinish();
         warm += 64;
         double t = now_ms();
         if (!top &&  t - start > 200) { /* a 0 read earlier is the previous process's */
            ged_freq(f0, sizeof(f0));
            if (!strncmp(f0, "0 ", 2))
               top = t;
         }
         if ((top && t - top >= warmup) || t - start > 5000)
            break;
      }
      ged_freq(f0, sizeof(f0));
      const double c0 = cpu_ms();
      clock_gettime(CLOCK_MONOTONIC, &t0);
      for (int it = 0; it < iterations; it++) {
         if (!blend)
            glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glFlush();
      }
      glFinish();
      clock_gettime(CLOCK_MONOTONIC, &t1);
      const double c1 = cpu_ms();
      ged_freq(f1, sizeof(f1));
      printf("wall gl %s %.4f ms/pass (%d passes, %d warm-up passes) freq %s -> %s, process cpu %.4f ms/pass\n", mode,
             ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / iterations, iterations, warm,
             f0, f1, (c1 - c0) / iterations);
   } else {
      for (int it = 0; it < iterations; it++) {
         if (!blend)
            glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         if (it == 0 && getenv("GLWORK_DUMP_MEM")) { /* XCLIPSE: the fragment code is written at draw time, before the kick */
            char sub[300];
            snprintf(sub, sizeof(sub), "%s/draw", getenv("GLWORK_DUMP_MEM"));
            mkdir(sub, 0755);
            dump_mem(sub);
         }
         glFinish();
      }
      if (getenv("GLWORK_DUMP_MEM")) {
         char sub[300];
         snprintf(sub, sizeof(sub), "%s/done", getenv("GLWORK_DUMP_MEM"));
         mkdir(sub, 0755);
         dump_mem(sub);
      }
   }
   const unsigned sum = readsum(GL_COLOR_ATTACHMENT0, BIG, BIG); /* before glGetError */
   printf("glwork bench %s%s: %d passes, sum %u, glGetError 0x%x\n", mode, f16tex ? " rgba16f" : "",
          iterations, sum, glGetError());

   const char *dump = getenv("GLWORK_DUMP_BINARY");
   if (dump) {
      GLint len = 0;
      glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
      void *bin = len > 0 ? malloc((size_t)len) : NULL;
      FILE *f = bin ? fopen(dump, "wb") : NULL;
      if (f) {
         GLenum format = 0;
         glGetProgramBinary(p, len, NULL, &format, bin);
         fwrite(bin, 1, (size_t)len, f);
         fclose(f);
         fprintf(stderr, "glwork bench: %d-byte program binary, format 0x%x, written to %s\n", len, format, dump);
      } else {
         fprintf(stderr, "glwork bench: no program binary (length %d)\n", len);
      }
      free(bin);
   }
   return 0;
}

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "clear";
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(d, NULL, NULL);
   const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE,
                         EGL_PBUFFER_BIT, EGL_NONE };
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(d, ca, &cfg, 1, &n);
   const EGLint sa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
   const EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE };
   EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
   if (!n || !s || !c || !eglMakeCurrent(d, s, s, c)) {
      fprintf(stderr, "egl setup failed: 0x%x\n", eglGetError());
      return 1;
   }

   GLuint tex[2], fbo[2];
   glGenTextures(2, tex);
   glGenFramebuffers(2, fbo);
   for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
   }
   glViewport(0, 0, W, H);
   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   static uint8_t pixels[W * H * 4];
   for (unsigned i = 0; i < sizeof(pixels); i++)
      pixels[i] = (uint8_t)(i * 7);

   GLuint draw = prog(GL_VERTEX_SHADER,
                      "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                      "gl_Position=vec4(p*2.0-1.0,0,1);}",
                      GL_FRAGMENT_SHADER,
                      "#version 300 es\nprecision highp float;uniform sampler2D t;out vec4 o;"
                      "void main(){o=texture(t,gl_FragCoord.xy/64.0)+vec4(0.25,0.5,0.75,1);}");
   GLuint comp = strcmp(mode, "compute") ? 0 :
      prog(GL_COMPUTE_SHADER,
           "#version 310 es\nlayout(local_size_x=64) in;layout(std430,binding=0) buffer B{uint v[];};"
           "void main(){v[gl_GlobalInvocationID.x]=gl_GlobalInvocationID.x*3u+1u;}",
           0, NULL);
   GLuint ssbo = 0;
   if (comp) {
      glGenBuffers(1, &ssbo);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
      glBufferData(GL_SHADER_STORAGE_BUFFER, 64 * 4, NULL, GL_DYNAMIC_COPY);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
   }

   if (!strcmp(mode, "bench"))
      return bench(argc, argv);

   int r = scene(mode, fbo[0]);
   if (r >= 0)
      return r;

   for (int it = 0; it < 3; it++) {
      if (!strcmp(mode, "clear")) {
         glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
         glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
      } else if (!strcmp(mode, "draw") || !strcmp(mode, "tex")) {
         if (!strcmp(mode, "tex")) {
            glBindTexture(GL_TEXTURE_2D, tex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
         }
         glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
         glUseProgram(draw);
         glBindTexture(GL_TEXTURE_2D, tex[1]);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      } else if (!strcmp(mode, "blit")) {
         glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
         glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]);
         glBlitFramebuffer(0, 0, W, H, 0, 0, W / 2, H / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
      } else if (!strcmp(mode, "compute")) {
         glUseProgram(comp);
         glDispatchCompute(1, 1, 1);
         glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
      } else {
         fprintf(stderr, "unknown mode %s\n", mode);
         return 2;
      }
      glFinish();
   }

   uint32_t check = 0;
   if (comp) {
      uint32_t *v = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 64 * 4, GL_MAP_READ_BIT);
      for (int i = 0; v && i < 64; i++)
         check += v[i];
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   } else {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      for (unsigned i = 0; i < sizeof(pixels); i++)
         check += pixels[i];
   }
   printf("glwork %s: checksum %u, glGetError 0x%x\n", mode, check, glGetError());
   return 0;
}
