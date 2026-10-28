#version 450
#extension GL_EXT_multiview : require
layout(location=0) in vec4 position;
layout(location=0) out float ordinary;
layout(location=7) out float eye;
layout(location=8) out float mixed;
layout(location=9) out float different_casts;
layout(location=10) out float escaped;
layout(location=11) out float same_expression;
layout(location=12) out vec2 packed;
void change(inout float value) { value = position.x; }
void main() {
    gl_Position = position;
    ordinary = position.x;
    eye = float(gl_ViewIndex);
    if (position.x > 0) {
        mixed = float(gl_ViewIndex);
        different_casts = float(gl_ViewIndex);
        same_expression = float(gl_ViewIndex);
    } else {
        mixed = position.y;
        different_casts = uintBitsToFloat(uint(gl_ViewIndex));
        same_expression = float(gl_ViewIndex);
    }
    float temporary = float(gl_ViewIndex);
    change(temporary);
    escaped = temporary;
    packed = vec2(float(gl_ViewIndex), position.x);
}
