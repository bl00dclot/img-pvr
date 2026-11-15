// SPDX-License-Identifier: MIT
#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { vec2 c = texture(t, uv).rg; o = vec4(c, 0.0, 1.0); }
