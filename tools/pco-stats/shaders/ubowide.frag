// SPDX-License-Identifier: MIT
#version 450
// XCLIPSE: wide constant-offset UBO reads. The vectoriser merges the 12 vec4 reads of v into loads of up to 16 dwords and
// the 6 scalars of s into one of 5 or 6; the shared-register copies of lane ubopush must split them (a repeat is at most 4).
// Regression: dEQP-VK.ubo.2_level_array.std140.vec4.vertex crashed the first phone build of the pass (comps[8]).
layout(set = 0, binding = 0, std140) uniform U { vec4 v[3][4]; float s[6]; };
layout(location = 0) out vec4 o;
void main()
{
   vec4 a = vec4(0.0);
   for (int i = 0; i < 4; i++)
      a += v[0][i] + v[1][i] + v[2][i];
   a.x += s[0] + s[1] + s[2] + s[3] + s[4] + s[5];
   o = a;
}
