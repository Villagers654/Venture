#version 450
// A multisampled and a single-sampled subpass input, read without ViewIndex
// (input_attachments.cpp adds it).
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInputMS color;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput depth;
layout(location = 0) out vec4 result;
void main() { result = subpassLoad(color, 1) + subpassLoad(depth).xxxx; }
