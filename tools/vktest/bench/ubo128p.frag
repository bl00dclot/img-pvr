// SPDX-License-Identifier: MIT
#version 450
#extension GL_GOOGLE_include_directive : require
// XCLIPSE: ubo128p: the same 128 B block as push constants (u128p).
layout(push_constant, std140) uniform P { vec4 v[5]; ivec4 m[3]; };
#include "ubo128_body.glsl"
