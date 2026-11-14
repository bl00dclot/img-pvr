// SPDX-License-Identifier: MIT
#version 450
// glwork big's vertex shader: triangle t covers half of a 2x2-cell square at cell t % 4096 of a 64x64 grid,
// coloured from t so the draw order shows in the result.
layout(location = 0) out vec4 c;
void main()
{
   int t = gl_VertexIndex / 3, v = gl_VertexIndex - t * 3, i = t & 4095;
   vec2 p = vec2(i & 63, i >> 6) / 32.0 - 1.0 + vec2(v == 1 ? 1.0 / 16.0 : 0.0, v == 2 ? 1.0 / 16.0 : 0.0);
   c = vec4(float(t & 255), float((t >> 8) & 255), float(t >> 16), 255.0) / 255.0;
   gl_Position = vec4(p, 0.0, 1.0);
}
