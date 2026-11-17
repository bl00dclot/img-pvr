// SPDX-License-Identifier: MIT
#version 450
#extension GL_GOOGLE_include_directive : require
// XCLIPSE: ubo128: the 128 B block as a uniform buffer (u128h, u128d) or an inline uniform block (u128i), binding 1.
layout(std140, set = 0, binding = 1) uniform U { vec4 v[5]; ivec4 m[3]; };
#include "ubo128_body.glsl"
