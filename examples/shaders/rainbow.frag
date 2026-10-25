#version 300 es
// Example custom fragment shader for 3gine. In a script:  engine.load_shader("rainbow.frag")
// Put the file in  <project>/shaders/  . Inputs and uniforms below are provided by the engine.
precision highp float;
in vec3 vN;          // world normal
in vec3 vW;          // world position
uniform vec3 uColor; // object colour
uniform vec3 uCam;   // camera position
uniform vec3 uSun;   // direction towards the sun
uniform vec3 uFog;
uniform float uTime;
uniform float uAlpha;   // < 1.0 means "blob shadow": output uColor with this alpha
out vec4 o;
void main() {
    if (uAlpha < 1.0) { o = vec4(uColor, uAlpha); return; }
    vec3 n = normalize(vN);
    float d = max(dot(n, normalize(uSun)), 0.0);
    vec3 rainbow = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + vW.y * 1.5 + uTime * 2.0);
    vec3 lit = mix(uColor, rainbow, 0.55) * (0.35 + 0.65 * d);
    float f = smoothstep(25.0, 85.0, length(uCam - vW));
    o = vec4(mix(lit, uFog, f), 1.0);
}
