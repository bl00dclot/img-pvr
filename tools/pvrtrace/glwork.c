// SPDX-License-Identifier: MIT
//
// glwork: tiny native GLES workloads for pvrtrace. Each mode makes the vendor GLES driver submit one
// kind of GPU work, so its client commands can be captured and compared with Mesa's for the same job.
//
//   glwork clear | draw | tex | blit | compute     (64x64 RGBA8 pbuffer-less FBO, 3 iterations)
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
