// SPDX-License-Identifier: MIT
#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }
