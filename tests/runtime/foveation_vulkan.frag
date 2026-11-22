#version 450
// Each fragment writes where it was shaded: a coarse fragment writes its
// centre into every pixel it covers.
layout(location = 0) out uint where;
void main() {
    where = uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 16);
}
