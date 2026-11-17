// SPDX-License-Identifier: MIT
#version 450
// XCLIPSE: ubofix. Two uniform blocks whose promoted ranges sit back to back: A is read as a vec3 (range of 3 dwords), B as a
// vec4 (range of 4 dwords), so B's shared registers start at (start of A) + 3 and the vec4 read is a repeat of 4 from an odd
// register. vkbench ubox (UBO_PAD=<n> moves A's start in steps of 4 dwords) puts that read across shared register 255 / 256.
layout(set = 0, binding = 0) uniform sampler2D t;
layout(std140, set = 0, binding = 1) uniform A { float a0, a1, a2, a3; };
layout(std140, set = 0, binding = 2) uniform B { float b0, b1, b2, b3, b4, b5; };
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   vec4 c = texture(t, uv);
   vec3 a = vec3(a0, a1, a2);
   vec4 b = vec4(b0, b1, b2, b3);
   o = vec4(clamp(c.rgb * a + b.rgb, 0.0, 1.0), b.a);
}
