#version 450
// Each fragment writes where it is (rate_layer_probe.cpp): a coarse fragment
// writes one colour to every pixel it covers.
layout(location = 0) out vec4 color;
void main() {
  color = vec4(gl_FragCoord.xy / 64.0, 0.0, 1.0);
}
