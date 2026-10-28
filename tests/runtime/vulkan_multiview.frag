#version 450
layout(location=7) in float eye;
layout(location=11) in float same_expression;
layout(location=0) out vec4 color;
void main() {
    color = uint(eye) == 1u && uint(same_expression) == 1u
        ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1);
}
