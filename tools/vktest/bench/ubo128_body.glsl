// SPDX-License-Identifier: MIT
// XCLIPSE: shared body of ubo128.frag / ubo128p.frag: the shape of ubo.frag (texture read, 12 branches on integer
// flags, scalar uniforms) with the whole block in 128 B of std140: vec4 v[5] (20 scalars) + ivec4 m[3] (12 flags).
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   vec4 c = texture(t, uv);
   vec3 a = c.rgb;
   if (m[0].x != 0) a = a * v[0].x + vec3(v[0].y);
   if (m[0].y != 0) a = a * v[0].z + vec3(v[0].w);
   if (m[0].z != 0) a = mix(a, vec3(v[1].x, v[1].y, v[1].z), v[1].w);
   if (m[0].w != 0) a = a * v[2].x + vec3(v[2].y);
   if (m[1].x != 0) a = a * v[2].z + vec3(v[2].w, v[3].x, v[3].y);
   if (m[1].y != 0) a = mix(a, vec3(v[3].z), v[3].w);
   if (m[1].z != 0) a = a * v[4].x + vec3(v[4].y);
   if (m[1].w != 0) a = a + vec3(v[4].z, v[4].w, v[0].x);
   if (m[2].x != 0) a = a * v[1].x + vec3(v[2].x);
   if (m[2].y != 0) a = mix(a, vec3(v[3].y, v[3].z, v[3].w), v[4].x);
   if (m[2].z != 0) a = a * v[0].w + vec3(v[1].y);
   if (m[2].w != 0) a = a + vec3(v[2].z, v[3].x, v[4].w);
   o = vec4(clamp(a, 0.0, 1.0), c.a);
}
