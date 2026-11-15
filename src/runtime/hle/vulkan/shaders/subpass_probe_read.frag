#version 450
// The probe's second subpass (subpass_input_probe.cpp) copies the first
// subpass's resolved and multisampled colors out with subpassLoad, which reads
// the current view's layer. A driver that gets this wrong shows view 0's red in
// view 1. As in a game, the shader does not name the view.
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput resolved;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInputMS multisampled;
layout(location = 0) out vec4 resolved_copy;
layout(location = 1) out vec4 multisampled_copy;
void main() {
  resolved_copy = subpassLoad(resolved);
  multisampled_copy = subpassLoad(multisampled, 0);
}
