// SPDX-License-Identifier: MIT
#version 450
#extension GL_GOOGLE_include_directive : require
// XCLIPSE: ubogap: ubo128p with the 12 flags as compile-time constants (the default pattern of vkbench, 9 on):
// what a compiler that folds uniform branches at pipeline creation (specialisation) would emit. v stays in push constants.
layout(push_constant, std140) uniform P { vec4 v[5]; };
const ivec4 m[3] = ivec4[3](ivec4(1, 0, 1, 1), ivec4(1, 1, 0, 1), ivec4(1, 1, 1, 0));
#include "ubo128_body.glsl"
