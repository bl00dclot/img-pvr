// SPDX-License-Identifier: MIT
#version 450
// XCLIPSE: uniform-driven code in the shape of the game's draw shader: the texture read, then branches on integer
// flags that read scalar uniforms at many offsets of one std140 UBO (the loose-uniform block of tools/pco-stats corpus).
layout(set = 0, binding = 0) uniform sampler2D t;
layout(std140, set = 0, binding = 1) uniform U { vec4 v[8]; ivec4 m[8]; float s[16]; };
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   vec4 c = texture(t, uv);
   vec3 a = c.rgb;
   if (m[0].x != 0) a = a * v[0].x + vec3(v[0].y);
   if (m[0].y != 0) a = a * v[1].x + vec3(v[1].z);
   if (m[1].x != 0) a = mix(a, vec3(v[2].x, v[2].y, v[2].z), s[0]);
   if (m[1].y != 0) a = a * s[1] + vec3(s[2]);
   if (m[2].x != 0) a = a * v[3].w + vec3(v[4].x);
   if (m[2].y != 0) a = mix(a, vec3(s[3], s[4], s[5]), v[4].y);
   if (m[3].x != 0) a = a * s[6] + vec3(s[7]);
   if (m[3].y != 0) a = a * v[5].x + vec3(v[5].y, v[6].x, v[6].y);
   if (m[4].x != 0) a = a * s[8] + vec3(s[9]);
   if (m[4].y != 0) a = mix(a, vec3(v[7].x), s[10]);
   if (m[5].x != 0) a = a * s[11] + vec3(s[12]);
   if (m[5].y != 0) a = a + vec3(s[13], s[14], s[15]);
   o = vec4(clamp(a, 0.0, 1.0), c.a);
}
