#version 450
// HairHarpy Toon's outline: the line's colour (linear)
layout(location = 0) out vec4 outColor;
layout(std140, set = 1, binding = 12) uniform Params { vec4 v[32]; } PB;
void main() { outColor = vec4(PB.v[17].rgb, 1.0); }
