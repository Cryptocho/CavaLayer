const char *vertex_shader_source = R"(
    #version 320 es
    precision highp float;
    layout(location = 0) in vec2 position;
    void main() {
        gl_Position = vec4(position, 0.0, 1.0);
    }
)";

const char *fragment_shader_source = R"(
    #version 320 es
    precision highp float;
    uniform vec4 colorTop;
    uniform vec4 colorBottom;
    uniform float screenHeight;
    out vec4 fragColor;
    void main() {
        float t = gl_FragCoord.y / screenHeight;
        fragColor = mix(colorBottom, colorTop, t);
    }
)";