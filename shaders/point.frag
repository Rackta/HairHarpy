#version 450
layout(location = 0) in vec3 vColor;
layout(location = 1) in float vShape;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 params;
  vec4 eye;
} pc;

// a Weld locks marker (shape 3 — '!', 4 — '✓'): a pin, its head round, its tip at the bottom of the sprite; the colour
// with a white rim and a white mark
float pinIn(vec2 p, float shrink) {
  float head = length(p - vec2(0.5, 0.33)) - (0.3 - shrink);
  float t = clamp((p.y - 0.33) / (0.97 - 0.33), 0.0, 1.0);
  float tail = (p.y < 0.33 || p.y > 0.97 - shrink) ? 1.0 : abs(p.x - 0.5) - (0.3 - shrink) * (1.0 - t);
  return min(head, tail);
}
float segD(vec2 p, vec2 a, vec2 b) { vec2 ab = b - a; float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0); return length(p - a - ab * t); }

void main() {
  if (vShape > 4.5) { outColor = vec4(vColor, pc.params.y); return; }   // (5 — a plain square: the Topology room's vertices)
  if (vShape > 2.5) {
    vec2 p = gl_PointCoord;
    if (pinIn(p, 0.0) > 0.0) discard;
    bool rim = pinIn(p, 0.06) > 0.0, mark;
    if (vShape < 3.5) mark = (abs(p.x - 0.5) < 0.045 && p.y > 0.15 && p.y < 0.4) || length(p - vec2(0.5, 0.49)) < 0.05;
    else mark = segD(p, vec2(0.36, 0.33), vec2(0.46, 0.44)) < 0.045 || segD(p, vec2(0.46, 0.44), vec2(0.64, 0.24)) < 0.045;
    outColor = vec4(rim || mark ? vec3(1.0) : vColor, pc.params.y);
    return;
  }
  vec2 d = gl_PointCoord - 0.5;
  float r = (vShape > 0.5 && vShape < 1.5) ? abs(d.x) + abs(d.y) : length(d);
  if (r > 0.5) discard;
  float a = pc.params.y * smoothstep(0.5, 0.42, r);
  if (vShape > 1.5) {
    if (r < 0.15) discard;
    vec3 c = (r > 0.38 || r < 0.22) ? vec3(0.06) : vColor;
    outColor = vec4(c, a); return;
  }
  vec3 c = r > 0.34 ? vec3(0.06) : vColor;
  outColor = vec4(c, a);
}
