// SPDX-License-Identifier: MIT
#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(t, uv).r, 0.0, 0.0, 1.0); }
