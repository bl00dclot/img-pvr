// SPDX-License-Identifier: MIT
#version 450
// glwork draw's colour with its (all-zero) texture term left out.
layout(location = 0) out vec4 o;
void main()
{
   o = vec4(0.25, 0.5, 0.75, 1.0);
}
