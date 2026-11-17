// SPDX-License-Identifier: MIT
#version 450
// XCLIPSE: consumes the vertex shader result of ubowide.vert so that the vertex shader's UBO reads stay.
layout(location = 0) in mediump float v_vtxResult;
layout(location = 0) out vec4 o;
void main() { o = vec4(v_vtxResult); }
