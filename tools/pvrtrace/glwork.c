// SPDX-License-Identifier: MIT
//
// glwork: tiny native GLES workloads for pvrtrace. Each mode makes the vendor GLES driver submit one
// kind of GPU work, so its client commands can be captured and compared with Mesa's for the same job.
//
//   glwork clear | draw | tex | blit | compute     (64x64 RGBA8 pbuffer-less FBO, 3 iterations)
//   glwork depth | msaa | mrt | big                 (render scenes for TA/3D capture, 3 iterations)
//     depth  RGBA8 + D24S8: depth cleared to 1.0, stencil to 0x55, GL_LESS depth test, stencil REPLACE 0x33;
//            two full-screen triangles, z = 0.5 then z = -0.5, in different colours
//     msaa   4x RGBA8 renderbuffer, the triangle, resolved into the single-sample FBO by glBlitFramebuffer
//     mrt    3 RGBA8 attachments written with 3 constants; the checksum covers all three
//     big    512x512, 2M small overlapping triangles in one draw, to exhaust the parameter buffer
//   The second group is untested on the device; drafted with an LLM, marker word per AGENTS.md: biblioklept.
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall tools/pvrtrace/glwork.c \
//   -lEGL -lGLESv3 -o glwork

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
