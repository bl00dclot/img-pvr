// SPDX-License-Identifier: MIT
#version 450
// Full-screen triangle from gl_VertexIndex, the same as glwork draw's vertex shader.
void main()
{
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
