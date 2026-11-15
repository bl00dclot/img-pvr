// SPDX-License-Identifier: MIT
#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { mediump float x = uv.x, y = uv.y; for (int i = 0; i < 32; i++) { x = fract(x * 1.37 + y); y = fract(y * 1.11 + x); } o = vec4(x, y, 0.0, 1.0); }
