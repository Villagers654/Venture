#version 450
#extension GL_EXT_multiview : require
// The probe's first subpass (subpass_input_probe.cpp): red in view 0, green in view 1.
layout(location = 0) out vec4 color;
void main() { color = gl_ViewIndex == 0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }
