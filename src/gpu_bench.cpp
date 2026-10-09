// SPDX-License-Identifier: MIT
//
//  gpu_bench.cpp
//
//  A self-contained OpenGL benchmark.  It creates a real X11/GLX window,
//  builds a high-polygon procedural scene (a torus-knot "hero" model plus a
//  large instanced ring and a tessellated floor), shades everything with a
//  physically based Cook-Torrance/GGX pipeline and applies an HDR bloom +
//  ACES tone-mapping post process.  Frame times are sampled while rendering.
//
#include "gpu_bench.hpp"
#include "util.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <epoxy/gl.h>
#include <epoxy/glx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

namespace bench {

// ===========================================================================
//  Minimal linear algebra
// ===========================================================================
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, 0};
}

struct Mat4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    static Mat4 identity() { return Mat4{}; }

    static Mat4 translate(Vec3 t) {
        Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r;
    }
    static Mat4 scale(float s) {
        Mat4 r; r.m[0] = r.m[5] = r.m[10] = s; return r;
    }
    static Mat4 rotateX(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; return r;
    }
    static Mat4 rotateY(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; return r;
    }
    static Mat4 rotateZ(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c; return r;
    }

    static Mat4 perspective(float fovY, float aspect, float znear, float zfar) {
        Mat4 r;
        float f = 1.0f / std::tan(fovY * 0.5f);
        for (float& v : r.m) v = 0.0f;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
        Vec3 f = normalize(center - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

// ===========================================================================
//  Shaders
// ===========================================================================
const char* kSceneVertex = R"GLSL(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform mat4 uInstances[32];
uniform int  uUseInstances;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out float vInstId;

void main() {
    mat4 model = uModel;
    if (uUseInstances == 1) {
        model = uInstances[gl_InstanceID];
        vInstId = float(gl_InstanceID);
    } else {
        vInstId = -1.0;
    }
    vec4 wp = model * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(model) * aNormal;
    vUV = aUV;
    gl_Position = uViewProj * wp;
}
)GLSL";

const char* kSceneFragment = R"GLSL(#version 430 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in float vInstId;

out vec4 FragColor;

uniform vec3  uCameraPos;
uniform vec3  uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3  uLightPos[8];
uniform vec3  uLightColor[8];
uniform float uLightIntensity[8];
uniform int   uLightCount;
uniform float uTime;
uniform int   uDetail;

const float PI = 3.14159265359;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

vec3 perturbNormal(vec3 N, vec3 p) {
    float e = 0.15;
    float n0 = vnoise(p * 4.0);
    float nx = vnoise((p + vec3(e, 0, 0)) * 4.0);
    float ny = vnoise((p + vec3(0, e, 0)) * 4.0);
    float nz = vnoise((p + vec3(0, 0, e)) * 4.0);
    vec3 g = vec3(nx - n0, ny - n0, nz - n0) / e;
    return normalize(N + g * 0.12);
}

float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float geometrySchlick(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlick(max(dot(N, V), 0.0), rough) *
           geometrySchlick(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec3 N = normalize(vNormal);
    if (uDetail == 1) {
        N = perturbNormal(N, vWorldPos * 1.5 + vec3(0.0, uTime * 0.08, 0.0));
    }
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 albedo = uBaseColor;
    // subtle per-instance hue variation using the instance id
    if (vInstId >= 0.0) {
        float h = fract(vInstId * 0.1618 + 0.13);
        albedo = mix(albedo, vec3(0.35 + 0.65 * h, 0.55 + 0.45 * fract(h * 3.1), 0.8 - 0.5 * h), 0.35);
    }
    float metallic = uMetallic;
    float roughness = clamp(uRoughness, 0.045, 1.0);
    vec3 F0 = mix(vec3(0.045), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < uLightCount; ++i) {
        vec3 toLight = uLightPos[i] - vWorldPos;
        float dist = length(toLight);
        vec3 L = toLight / max(dist, 1e-4);
        float atten = 1.0 / (1.0 + 0.11 * dist + 0.028 * dist * dist);
        vec3 radiance = uLightColor[i] * uLightIntensity[i] * atten;

        vec3 H = normalize(V + L);
        float NDF = distributionGGX(N, H, roughness);
        float G = geometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 spec = (NDF * G * F) /
                    max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 1e-4);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
        Lo += (kD * albedo / PI + spec) * radiance * max(dot(N, L), 0.0);
    }

    // cheap image based-ish ambient with a sky/ground gradient
    vec3 sky = mix(vec3(0.10, 0.13, 0.20), vec3(0.02, 0.03, 0.05), 0.5 + 0.5 * N.y);
    vec3 color = Lo + albedo * sky;

    FragColor = vec4(color, 1.0);
}
)GLSL";

const char* kFullscreenVertex = R"GLSL(#version 430 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* kBrightFragment = R"GLSL(#version 430 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHdr;
uniform float uThreshold;
void main() {
    vec3 c = texture(uHdr, vUV).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float f = max(l - uThreshold, 0.0) / max(l, 1e-4);
    FragColor = vec4(c * f, 1.0);
}
)GLSL";

const char* kBlurFragment = R"GLSL(#version 430 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uImage;
uniform vec2 uTexel;
uniform vec2 uDir;
void main() {
    float w0 = 0.227027, w1 = 0.1945946, w2 = 0.1216216, w3 = 0.054054, w4 = 0.016216;
    vec3 sum = texture(uImage, vUV).rgb * w0;
    vec2 o;
    o = uDir * uTexel * 1.5;
    sum += texture(uImage, vUV + o).rgb * w1;
    sum += texture(uImage, vUV - o).rgb * w1;
    o = uDir * uTexel * 3.0;
    sum += texture(uImage, vUV + o).rgb * w2;
    sum += texture(uImage, vUV - o).rgb * w2;
    o = uDir * uTexel * 4.5;
    sum += texture(uImage, vUV + o).rgb * w3;
    sum += texture(uImage, vUV - o).rgb * w3;
    o = uDir * uTexel * 6.0;
    sum += texture(uImage, vUV + o).rgb * w4;
    sum += texture(uImage, vUV - o).rgb * w4;
    FragColor = vec4(sum, 1.0);
}
)GLSL";

const char* kCompositeFragment = R"GLSL(#version 430 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHdr;
uniform sampler2D uBloom;
uniform float uExposure;
uniform float uBloomStrength;
uniform float uTime;
uniform vec2  uResolution;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 bloom = texture(uBloom, vUV).rgb;
    vec3 c = hdr + bloom * uBloomStrength;
    c *= uExposure;
    c = aces(c);
    vec2 uv = vUV * 2.0 - 1.0;
    c *= 1.0 - dot(uv, uv) * 0.22;
    c += (hash12(vUV * uResolution + fract(uTime)) - 0.5) * 0.008;
    FragColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
)GLSL";

// ===========================================================================
//  GL helpers
// ===========================================================================
GLuint compileShader(GLenum type, const char* src, std::string& error) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        error = log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint linkProgram(const char* vs, const char* fs, std::string& error) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs, error);
    if (!v) return 0;
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs, error);
    if (!f) { glDeleteShader(v); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        error = log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

struct RenderTarget {
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;
    int width = 0;
    int height = 0;
};

RenderTarget createRenderTarget(int w, int h, bool withDepth) {
    RenderTarget rt;
    rt.width = w;
    rt.height = h;
    glGenTextures(1, &rt.color);
    glBindTexture(GL_TEXTURE_2D, rt.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);
    if (withDepth) {
        glGenRenderbuffers(1, &rt.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, rt.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.depth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return rt;
}

// ===========================================================================
//  Procedural geometry
// ===========================================================================
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Sweep a circular tube along a (P, Q) torus knot lying near the XZ plane.
Mesh makeTorusKnot(int along, int radial, float tubeR) {
    Mesh mesh;
    const int P = 3, Q = 7;
    const float period = 2.0f * kPi * static_cast<float>(P);
    const float R = 1.05f;
    const float r = 0.55f;
    const float rho = 0.62f; // vertical amplitude

    auto curve = [&](float u) {
        float cu = std::cos(u), su = std::sin(u);
        float a = static_cast<float>(Q) / static_cast<float>(P) * u;
        float ca = std::cos(a), sa = std::sin(a);
        return Vec3{(R + r * ca) * cu, rho * sa, (R + r * ca) * su};
    };

    mesh.vertices.reserve(static_cast<size_t>(along + 1) * (radial + 1));
    for (int i = 0; i <= along; ++i) {
        float u = period * static_cast<float>(i) / static_cast<float>(along);
        Vec3 p0 = curve(u);
        Vec3 p1 = curve(u + 1e-3f);
        Vec3 T = normalize(p1 - p0);
        Vec3 up{0, 1, 0};
        Vec3 N = normalize(cross(T, up));
        if (!(dot(N, N) > 1e-6f)) N = normalize(cross(T, Vec3{1, 0, 0}));
        Vec3 B = cross(T, N);
        for (int j = 0; j <= radial; ++j) {
            float v = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(radial);
            Vec3 dir = N * std::cos(v) + B * std::sin(v);
            Vec3 pos = p0 + dir * tubeR;
            Vertex vert{};
            vert.px = pos.x; vert.py = pos.y; vert.pz = pos.z;
            vert.nx = dir.x; vert.ny = dir.y; vert.nz = dir.z;
            vert.u = static_cast<float>(i) / static_cast<float>(along) * 8.0f;
            vert.v = static_cast<float>(j) / static_cast<float>(radial) * 2.0f;
            mesh.vertices.push_back(vert);
        }
    }
    auto idx = [&](int i, int j) { return static_cast<uint32_t>(i * (radial + 1) + j); };
    for (int i = 0; i < along; ++i) {
        for (int j = 0; j < radial; ++j) {
            uint32_t a = idx(i, j), b = idx(i + 1, j), c = idx(i + 1, j + 1), d = idx(i, j + 1);
            mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
            mesh.indices.push_back(a); mesh.indices.push_back(c); mesh.indices.push_back(d);
        }
    }
    return mesh;
}

Mesh makePlane(int segments, float halfSize) {
    Mesh mesh;
    mesh.vertices.reserve(static_cast<size_t>(segments + 1) * (segments + 1));
    for (int z = 0; z <= segments; ++z) {
        for (int x = 0; x <= segments; ++x) {
            float fx = static_cast<float>(x) / static_cast<float>(segments);
            float fz = static_cast<float>(z) / static_cast<float>(segments);
            float px = (fx * 2.0f - 1.0f) * halfSize;
            float pz = (fz * 2.0f - 1.0f) * halfSize;
            Vertex v{};
            v.px = px; v.py = -2.6f; v.pz = pz;
            v.nx = 0; v.ny = 1; v.nz = 0;
            v.u = fx * 40.0f; v.v = fz * 40.0f;
            mesh.vertices.push_back(v);
        }
    }
    auto idx = [&](int x, int z) { return static_cast<uint32_t>(z * (segments + 1) + x); };
    for (int z = 0; z < segments; ++z) {
        for (int x = 0; x < segments; ++x) {
            uint32_t a = idx(x, z), b = idx(x + 1, z), c = idx(x + 1, z + 1), d = idx(x, z + 1);
            mesh.indices.push_back(a); mesh.indices.push_back(d); mesh.indices.push_back(c);
            mesh.indices.push_back(a); mesh.indices.push_back(c); mesh.indices.push_back(b);
        }
    }
    return mesh;
}

struct GlMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei indexCount = 0;
};

GlMesh uploadMesh(const Mesh& m) {
    GlMesh g;
    g.indexCount = static_cast<GLsizei>(m.indices.size());
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m.vertices.size() * sizeof(Vertex)),
                 m.vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &g.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizei>(m.indices.size() * sizeof(uint32_t)),
                 m.indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, px));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return g;
}

// ===========================================================================
//  X11 / GLX window
// ===========================================================================
struct GlWindow {
    Display* dpy = nullptr;
    Window win = 0;
    GLXContext ctx = nullptr;
    Colormap cmap = 0;
    Atom wmDelete = 0;
    int width = 0, height = 0;
    bool visible = true;
    std::string error;
    bool ok = false;

    bool open(int w, int h, bool visible_) {
        width = w;
        height = h;
        visible = visible_;
        dpy = XOpenDisplay(nullptr);
        if (!dpy) { error = "cannot open X display (is DISPLAY set?)"; return false; }

        int screen = DefaultScreen(dpy);
        Window root = RootWindow(dpy, screen);

        int glxMajor = 0, glxMinor = 0;
        if (!glXQueryVersion(dpy, &glxMajor, &glxMinor) ||
            (glxMajor == 1 && glxMinor < 3)) {
            error = "GLX 1.3+ required";
            return false;
        }

        auto chooseFB = [&](bool msaa) {
            std::vector<int> attribs = {
                GLX_X_RENDERABLE, True,
                GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                GLX_RENDER_TYPE, GLX_RGBA_BIT,
                GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
                GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
                GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8,
                GLX_DOUBLEBUFFER, True,
            };
            if (msaa) { attribs.push_back(GLX_SAMPLE_BUFFERS); attribs.push_back(1);
                        attribs.push_back(GLX_SAMPLES); attribs.push_back(4); }
            attribs.push_back(None);
            int count = 0;
            GLXFBConfig* cfgs = glXChooseFBConfig(dpy, screen, attribs.data(), &count);
            GLXFBConfig chosen = nullptr;
            if (cfgs && count > 0) chosen = cfgs[0];
            if (cfgs) XFree(cfgs);
            return chosen;
        };

        GLXFBConfig fbc = chooseFB(true);
        if (!fbc) fbc = chooseFB(false);
        if (!fbc) { error = "no suitable GLX framebuffer configuration"; return false; }

        XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc);
        if (!vi) { error = "no visual for framebuffer config"; return false; }

        cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
        XSetWindowAttributes swa{};
        swa.colormap = cmap;
        swa.background_pixmap = None;
        swa.border_pixel = 0;
        swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask |
                         PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
        win = XCreateWindow(dpy, root, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h),
                            0, vi->depth, InputOutput, vi->visual,
                            CWColormap | CWBackPixmap | CWBorderPixel | CWEventMask, &swa);
        XFree(vi);
        if (!win) { error = "failed to create window"; return false; }

        wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wmDelete, 1);
        setTitle("GPU Benchmark");

        auto createCtx = [&](int major, int minor) -> GLXContext {
            int attribs[] = {
                GLX_CONTEXT_MAJOR_VERSION_ARB, major,
                GLX_CONTEXT_MINOR_VERSION_ARB, minor,
                GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                None
            };
            return glXCreateContextAttribsARB(dpy, fbc, nullptr, True, attribs);
        };
        ctx = createCtx(4, 5);
        if (!ctx) ctx = createCtx(4, 3);
        if (!ctx)  ctx = glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, nullptr, True);
        if (!ctx) { error = "failed to create an OpenGL 4.3+ context"; return false; }

        if (visible) {
            XMapWindow(dpy, win);
        }
        glXMakeCurrent(dpy, win, ctx);
        ok = true;
        return true;
    }

    void setTitle(const std::string& s) {
        if (dpy && win) {
            XStoreName(dpy, win, s.c_str());
            XFlush(dpy);
        }
    }

    void swap() { if (dpy && win) glXSwapBuffers(dpy, win); }

    void resizeIfNeeded() {
        XWindowAttributes wa{};
        if (XGetWindowAttributes(dpy, win, &wa) && wa.width > 0 && wa.height > 0) {
            width = wa.width;
            height = wa.height;
        }
    }

    void close() {
        if (dpy) {
            glXMakeCurrent(dpy, None, nullptr);
            if (ctx) glXDestroyContext(dpy, ctx);
            if (win) XDestroyWindow(dpy, win);
            if (cmap) XFreeColormap(dpy, cmap);
            XCloseDisplay(dpy);
        }
        dpy = nullptr;
        ctx = nullptr;
        win = 0;
    }
};

struct InputState {
    bool quit = false;
    bool paused = false;
    bool wireframe = false;
    float yaw = -0.6f;
    float pitch = 0.35f;
    float zoom = 1.0f;
    bool dragging = false;
    int lastX = 0, lastY = 0;
};

void pumpEvents(GlWindow& w, InputState& in) {
    if (!w.dpy) return;
    while (XPending(w.dpy) > 0) {
        XEvent ev;
        XNextEvent(w.dpy, &ev);
        switch (ev.type) {
            case ClientMessage:
                if (static_cast<Atom>(ev.xclient.data.l[0]) == w.wmDelete) in.quit = true;
                break;
            case DestroyNotify:
                in.quit = true;
                break;
            case ConfigureNotify:
                w.width = std::max(1, ev.xconfigure.width);
                w.height = std::max(1, ev.xconfigure.height);
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Escape) in.quit = true;
                else if (ks == XK_space) in.paused = !in.paused;
                else if (ks == XK_w) in.wireframe = !in.wireframe;
                else if (ks == XK_r) { in.yaw = -0.6f; in.pitch = 0.35f; in.zoom = 1.0f; }
                else if (ks == XK_plus || ks == XK_equal) in.zoom = std::max(0.4f, in.zoom * 0.92f);
                else if (ks == XK_minus) in.zoom = std::min(3.0f, in.zoom * 1.08f);
                break;
            }
            case ButtonPress:
                if (ev.xbutton.button == Button4) in.zoom = std::max(0.4f, in.zoom * 0.92f);
                else if (ev.xbutton.button == Button5) in.zoom = std::min(3.0f, in.zoom * 1.08f);
                else { in.dragging = true; in.lastX = ev.xbutton.x; in.lastY = ev.xbutton.y; }
                break;
            case ButtonRelease:
                in.dragging = false;
                break;
            case MotionNotify:
                if (in.dragging) {
                    in.yaw -= static_cast<float>(ev.xmotion.x - in.lastX) * 0.005f;
                    in.pitch += static_cast<float>(ev.xmotion.y - in.lastY) * 0.005f;
                    in.pitch = std::clamp(in.pitch, -1.4f, 1.4f);
                    in.lastX = ev.xmotion.x;
                    in.lastY = ev.xmotion.y;
                }
                break;
            default:
                break;
        }
    }
}

// ===========================================================================
//  Device query
// ===========================================================================
void readDeviceInfo(GpuDeviceInfo& info) {
    auto str = [](GLenum e) {
        const GLubyte* s = glGetString(e);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    info.vendor  = str(GL_VENDOR);
    info.renderer = str(GL_RENDERER);
    info.version = str(GL_VERSION);
    info.glsl    = str(GL_SHADING_LANGUAGE_VERSION);
    glGetIntegerv(GL_MAJOR_VERSION, &info.major);
    glGetIntegerv(GL_MINOR_VERSION, &info.minor);
    info.contextFlavor = "Core";
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &info.maxVertexAttribs);
    if (epoxy_has_gl_extension("GL_ARB_uniform_buffer_object")) {
        GLint v = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
        info.maxUniformBlockSize = v;
    }
    if (epoxy_has_gl_extension("GL_ARB_texture_multisample")) {
        GLint v = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &v);
        info.maxSamples = v;
    }
    if (epoxy_has_gl_extension("GL_NVX_gpu_memory_info")) {
        const GLenum DEDICATED = 0x9047, TOTAL_AVAIL = 0x9048, CUR_AVAIL = 0x9049;
        GLint dedicated = 0, total = 0, current = 0;
        glGetIntegerv(DEDICATED, &dedicated);
        glGetIntegerv(TOTAL_AVAIL, &total);
        glGetIntegerv(CUR_AVAIL, &current);
        info.vramTotalBytes = static_cast<uint64_t>(dedicated) * 1024ull;
        info.vramAvailableBytes = static_cast<uint64_t>(current) * 1024ull;
        info.vramKnown = dedicated > 0;
    } else if (epoxy_has_gl_extension("GL_ATI_meminfo")) {
        const GLenum FREE_MEM = 0x87FB; // GL_TEXTURE_FREE_MEMORY_ATI
        GLint v[4] = {0, 0, 0, 0};
        glGetIntegerv(FREE_MEM, v);
        info.vramTotalBytes = static_cast<uint64_t>(v[0]) * 1024ull;
        info.vramAvailableBytes = static_cast<uint64_t>(v[0]) * 1024ull;
        info.vramKnown = v[0] > 0;
    }
}

// Dump the current default framebuffer to a binary PPM (P6) image.
bool writePpmFramebuffer(const std::string& path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    // OpenGL's origin is bottom-left, PPM wants top-left: flip the rows.
    for (int y = h - 1; y >= 0; --y) {
        std::fwrite(pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3,
                    1, static_cast<size_t>(w) * 3, f);
    }
    std::fclose(f);
    return true;
}

} // namespace

bool queryGpuDevice(GpuDeviceInfo& out, std::string& error) {
    GlWindow w;
    if (!w.open(64, 64, false)) { error = w.error; return false; }
    readDeviceInfo(out);
    w.close();
    return true;
}

std::string gpuScoreRating(double score) {
    if (score >= 2500) return "Outstanding";
    if (score >= 1800) return "Excellent";
    if (score >= 1300) return "Very good";
    if (score >= 900)  return "Good";
    if (score >= 550)  return "Average";
    if (score >= 300)  return "Below average";
    return "Entry level";
}

// ===========================================================================
//  Benchmark
// ===========================================================================
bool runGpuBenchmark(const GpuOptions& opts, GpuBenchResult& out, std::string& error) {
    GlWindow w;
    if (!w.open(opts.width, opts.height, opts.showWindow)) {
        error = w.error;
        return false;
    }

    // vsync control (best effort)
    if (epoxy_has_glx_extension(w.dpy, DefaultScreen(w.dpy), "GLX_EXT_swap_control")) {
        glXSwapIntervalEXT(w.dpy, w.win, opts.vsync ? 1 : 0);
    } else if (epoxy_has_glx_extension(w.dpy, DefaultScreen(w.dpy), "GLX_MESA_swap_control")) {
        glXSwapIntervalMESA(opts.vsync ? 1 : 0);
    }

    readDeviceInfo(out.device);

    // ---- shaders ----------------------------------------------------------
    GLuint sceneProg = linkProgram(kSceneVertex, kSceneFragment, error);
    if (!sceneProg) { error = "scene shader: " + error; w.close(); return false; }
    GLuint brightProg = linkProgram(kFullscreenVertex, kBrightFragment, error);
    GLuint blurProg   = linkProgram(kFullscreenVertex, kBlurFragment, error);
    GLuint compProg   = linkProgram(kFullscreenVertex, kCompositeFragment, error);
    if (!brightProg || !blurProg || !compProg) {
        error = "post shader: " + error;
        w.close();
        return false;
    }

    // ---- geometry ---------------------------------------------------------
    const int quality = std::clamp(opts.quality, 1, 4);
    struct Preset { int along; int radial; int instances; };
    const Preset presets[5] = {
        {0, 0, 0},
        {256, 48, 8},
        {640, 112, 12},
        {1280, 192, 20},
        {2560, 320, 30},
    };
    const Preset& P = presets[quality];
    int instances = opts.instances > 0 ? std::min(opts.instances, 32) : P.instances;
    instances = std::clamp(instances, 1, 32);

    Mesh knotMesh = makeTorusKnot(P.along, P.radial, 0.16f);
    Mesh floorMesh = makePlane(quality >= 3 ? 400 : 256, 26.0f);
    GlMesh knot = uploadMesh(knotMesh);
    GlMesh floor = uploadMesh(floorMesh);

    // fullscreen triangle VAO
    GLuint fullVao = 0;
    glGenVertexArrays(1, &fullVao);

    out.width = w.width;
    out.height = w.height;
    out.quality = quality;
    out.instances = instances;
    out.trianglesPerFrame = static_cast<uint64_t>(knot.indexCount / 3) * static_cast<uint64_t>(instances)
                          + static_cast<uint64_t>(floor.indexCount / 3);
    out.verticesPerFrame = static_cast<uint64_t>(knotMesh.vertices.size()) * static_cast<uint64_t>(instances)
                         + static_cast<uint64_t>(floorMesh.vertices.size());
    out.drawCallsPerFrame = 2;

    RenderTarget hdr = createRenderTarget(w.width, w.height, true);
    int bw = std::max(1, w.width / 2), bh = std::max(1, w.height / 2);
    RenderTarget bloomA = createRenderTarget(bw, bh, false);
    RenderTarget bloomB = createRenderTarget(bw, bh, false);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // validated on the HDR target below anyway
    }
    glBindFramebuffer(GL_FRAMEBUFFER, hdr.fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error = "incomplete HDR framebuffer";
        w.close();
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    if (opts.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    // ---- uniforms ---------------------------------------------------------
    auto u = [](GLuint p, const char* n) { return glGetUniformLocation(p, n); };
    GLint uViewProj = u(sceneProg, "uViewProj");
    GLint uModel = u(sceneProg, "uModel");
    GLint uInstances = u(sceneProg, "uInstances");
    GLint uUseInstances = u(sceneProg, "uUseInstances");
    GLint uCameraPos = u(sceneProg, "uCameraPos");
    GLint uBaseColor = u(sceneProg, "uBaseColor");
    GLint uMetallic = u(sceneProg, "uMetallic");
    GLint uRoughness = u(sceneProg, "uRoughness");
    GLint uLightPos = u(sceneProg, "uLightPos");
    GLint uLightColor = u(sceneProg, "uLightColor");
    GLint uLightIntensity = u(sceneProg, "uLightIntensity");
    GLint uLightCount = u(sceneProg, "uLightCount");
    GLint uTime = u(sceneProg, "uTime");
    GLint uDetail = u(sceneProg, "uDetail");

    GLint bHdr = u(brightProg, "uHdr");
    GLint bThreshold = u(brightProg, "uThreshold");
    GLint blImage = u(blurProg, "uImage");
    GLint blTexel = u(blurProg, "uTexel");
    GLint blDir = u(blurProg, "uDir");
    GLint cHdr = u(compProg, "uHdr");
    GLint cBloom = u(compProg, "uBloom");
    GLint cExposure = u(compProg, "uExposure");
    GLint cBloomStrength = u(compProg, "uBloomStrength");
    GLint cTime = u(compProg, "uTime");
    GLint cResolution = u(compProg, "uResolution");

    // ---- scene constants --------------------------------------------------
    const int lightCount = 6;
    Vec3 lightColors[8] = {
        {1.0f, 0.85f, 0.65f}, {0.35f, 0.6f, 1.0f}, {1.0f, 0.35f, 0.5f},
        {0.45f, 1.0f, 0.75f}, {1.0f, 0.95f, 0.4f}, {0.7f, 0.5f, 1.0f},
        {1, 1, 1}, {1, 1, 1},
    };
    float lightIntensity[8] = {42.0f, 34.0f, 30.0f, 26.0f, 24.0f, 24.0f, 0.0f, 0.0f};

    InputState input;
    std::vector<float> instanceData(32 * 16, 0.0f);

    auto buildInstances = [&](float time) {
        for (int i = 0; i < instances; ++i) {
            Mat4 model;
            if (instances == 1) {
                model = Mat4::rotateY(time * 0.5f) * Mat4::scale(1.0f);
            } else if (i == 0) {
                model = Mat4::rotateY(time * 0.4f) * Mat4::rotateX(0.3f) * Mat4::scale(1.35f);
            } else {
                int ringCount = instances - 1;
                float t = ringCount > 1 ? static_cast<float>(i - 1) / static_cast<float>(ringCount) : 0.0f;
                float ang = t * 2.0f * kPi + time * 0.15f;
                float ringR = 3.4f;
                float scale = 0.5f + 0.12f * std::sin(i * 2.3f);
                Vec3 pos{std::cos(ang) * ringR, 0.35f + 0.5f * std::sin(i * 1.7f), std::sin(ang) * ringR};
                model = Mat4::translate(pos) *
                        Mat4::rotateY(ang + time * 0.35f) *
                        Mat4::rotateX(time * 0.6f + i) *
                        Mat4::rotateZ(time * 0.25f + i * 0.5f) *
                        Mat4::scale(scale);
            }
            std::memcpy(&instanceData[static_cast<size_t>(i) * 16], model.m, sizeof(float) * 16);
        }
    };

    // ---- frame function ---------------------------------------------------
    auto renderFrame = [&](float time, float exposure, bool cursorLight) {
        w.resizeIfNeeded();
        int fw = w.width, fh = w.height;

        // camera
        Vec3 center{0, 0, 0};
        float dist = 11.0f * input.zoom;
        Vec3 eye{
            center.x + dist * std::cos(input.pitch) * std::sin(input.yaw),
            center.y + dist * std::sin(input.pitch),
            center.z + dist * std::cos(input.pitch) * std::cos(input.yaw),
        };
        Mat4 view = Mat4::lookAt(eye, center, Vec3{0, 1, 0});
        Mat4 proj = Mat4::perspective(55.0f * kPi / 180.0f,
                                      static_cast<float>(fw) / static_cast<float>(std::max(1, fh)),
                                      0.1f, 200.0f);
        Mat4 viewProj = proj * view;

        buildInstances(time);

        // lights orbit the scene
        Vec3 lightPos[8];
        for (int i = 0; i < lightCount; ++i) {
            float ang = time * (0.4f + 0.13f * i) + i * 1.7f;
            float radius = 4.0f + 1.2f * std::sin(i * 2.1f);
            float y = 2.0f + 2.5f * std::sin(time * 0.5f + i);
            lightPos[i] = Vec3{std::cos(ang) * radius, y, std::sin(ang) * radius};
        }

        // ---- geometry pass into HDR ----
        glBindFramebuffer(GL_FRAMEBUFFER, hdr.fbo);
        glViewport(0, 0, fw, fh);
        glClearColor(0.01f, 0.012f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(sceneProg);
        glUniformMatrix4fv(uViewProj, 1, GL_FALSE, viewProj.m);
        glUniform3f(uCameraPos, eye.x, eye.y, eye.z);
        glUniform1i(uLightCount, lightCount);
        glUniform3fv(uLightPos, lightCount, reinterpret_cast<float*>(lightPos));
        glUniform3fv(uLightColor, lightCount, reinterpret_cast<float*>(lightColors));
        glUniform1fv(uLightIntensity, lightCount, lightIntensity);
        glUniform1f(uTime, time);
        glUniform1i(uDetail, 1);

        // floor
        glUniform1i(uUseInstances, 0);
        Mat4 floorModel = Mat4::identity();
        glUniformMatrix4fv(uModel, 1, GL_FALSE, floorModel.m);
        glUniform3f(uBaseColor, 0.020f, 0.024f, 0.032f);
        glUniform1f(uMetallic, 0.05f);
        glUniform1f(uRoughness, 0.9f);
        glBindVertexArray(floor.vao);
        glDrawElements(GL_TRIANGLES, floor.indexCount, GL_UNSIGNED_INT, nullptr);

        // hero + ring
        glUniform1i(uUseInstances, 1);
        glUniformMatrix4fv(uInstances, instances, GL_FALSE, instanceData.data());
        glUniform3f(uBaseColor, 0.95f, 0.62f, 0.25f);
        glUniform1f(uMetallic, 0.9f);
        glUniform1f(uRoughness, 0.22f);
        glBindVertexArray(knot.vao);
        glDrawElementsInstanced(GL_TRIANGLES, knot.indexCount, GL_UNSIGNED_INT, nullptr, instances);

        // ---- bloom ----
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(fullVao);

        // bright pass -> bloomA
        glBindFramebuffer(GL_FRAMEBUFFER, bloomA.fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(brightProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdr.color);
        glUniform1i(bHdr, 0);
        glUniform1f(bThreshold, 1.15f);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // separable blur, ping-pong between the two half-res targets
        glUseProgram(blurProg);
        glUniform2f(blTexel, 1.0f / static_cast<float>(bw), 1.0f / static_cast<float>(bh));
        glUniform1i(blImage, 0);
        RenderTarget* read = &bloomA;
        RenderTarget* write = &bloomB;
        for (int i = 0; i < opts.bloomIterations; ++i) {
            // horizontal pass: read -> write
            glBindFramebuffer(GL_FRAMEBUFFER, write->fbo);
            glViewport(0, 0, bw, bh);
            glBindTexture(GL_TEXTURE_2D, read->color);
            glUniform2f(blDir, 1.0f, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::swap(read, write);
            // vertical pass: read -> write
            glBindFramebuffer(GL_FRAMEBUFFER, write->fbo);
            glViewport(0, 0, bw, bh);
            glBindTexture(GL_TEXTURE_2D, read->color);
            glUniform2f(blDir, 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::swap(read, write);
        }
        GLuint bloomResult = read->color;

        // ---- composite to default framebuffer ----
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fw, fh);
        glUseProgram(compProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdr.color);
        glUniform1i(cHdr, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomResult);
        glUniform1i(cBloom, 1);
        glUniform1f(cExposure, exposure);
        glUniform1f(cBloomStrength, 0.45f);
        glUniform1f(cTime, time);
        glUniform2f(cResolution, static_cast<float>(fw), static_cast<float>(fh));
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glEnable(GL_DEPTH_TEST);
        return std::make_pair(fw, fh);
    };

    Timer runTimer;
    std::vector<double> frameTimes;
    double lastFpsTime = 0.0;
    int framesSince = 0;
    double fps = 0.0;
    float exposure = 0.85f;

    if (opts.interactive) {
        // Warm up and print instructions.
        std::printf("%s\n", paint("Interactive mode - drag to rotate, scroll to zoom, SPACE pause, W wireframe, ESC quit.", ansi::cyan()).c_str());
        std::fflush(stdout);
    }

    const double duration = opts.seconds;

    // Warm-up: compile shaders, fill caches and let the GPU clock ramp up.
    // Nothing here is measured.
    {
        double warmStart = nowSeconds();
        double warmSeconds = opts.interactive ? 0.0 : 1.0;
        while (!input.quit && nowSeconds() - warmStart < warmSeconds) {
            pumpEvents(w, input);
            if (input.quit) break;
            renderFrame(static_cast<float>(nowSeconds()), exposure, true);
            w.swap();
            glFinish();
        }
    }

    double start = nowSeconds();
    double lastTitle = -1.0;
    int totalFrames = 0;

    // Optional screenshot of the fully rendered frame.
    if (!opts.screenshotPath.empty()) {
        renderFrame(1.5f, exposure, true);
        glFinish();
        if (writePpmFramebuffer(opts.screenshotPath, w.width, w.height))
            std::printf("  %sScreenshot saved: %s%s\n\n", ansi::green(),
                        opts.screenshotPath.c_str(), ansi::reset());
        else
            std::fprintf(stderr, "  error: could not write screenshot %s\n", opts.screenshotPath.c_str());
        w.swap();
    }

    while (!input.quit) {
        pumpEvents(w, input);
        if (input.quit) break;

        double t = nowSeconds();
        double elapsed = t - start;

        if (input.paused && !opts.interactive) break; // pause aborts the timed run
        if (!opts.interactive && elapsed >= duration) break;

        float sceneTime = input.paused ? 0.0f : static_cast<float>(t);
        renderFrame(sceneTime, exposure, true);
        w.swap();
        if (!opts.interactive) glFinish(); // honest GPU completion time per frame

        double frameEnd = nowSeconds();
        double ft = frameEnd - t;
        if (ft > 0.0 && !input.paused) {
            frameTimes.push_back(ft * 1000.0);
            ++totalFrames;
            ++framesSince;
        }

        // FPS + title update roughly 4x / second
        if (t - lastTitle > 0.25) {
            double window = t - lastFpsTime;
            if (lastFpsTime > 0.0 && framesSince > 0) fps = framesSince / window;
            lastFpsTime = t;
            framesSince = 0;
            lastTitle = t;

            if (!frameTimes.empty()) {
                std::vector<double> recent(frameTimes.end() - std::min<size_t>(frameTimes.size(), 30),
                                           frameTimes.end());
                double avg = 0.0;
                for (double v : recent) avg += v;
                avg /= static_cast<double>(recent.size());
                if (avg > 0.0) fps = 1000.0 / avg;
            }

            std::ostringstream title;
            title << "GPU Benchmark  |  " << out.device.renderer << "  |  "
                  << w.width << "x" << w.height << "  |  "
                  << out.trianglesPerFrame / 1000000.0 << "M tris  |  "
                  << fixed(fps, 1) << " FPS  |  "
                  << fixed(1000.0 / std::max(fps, 1e-3), 2) << " ms";
            w.setTitle(title.str());

            if (!opts.interactive && !opts.suppressProgress) {
                std::printf("\r  %s  %5.1fs / %.0fs   %s %6.1f FPS   %s %6.2f ms   %s %6.1f M tris/frame   ",
                            paint("laufend", ansi::yellow()).c_str(),
                            elapsed, duration,
                            paint("FPS", ansi::gray()).c_str(), fps,
                            paint("frame", ansi::gray()).c_str(), 1000.0 / std::max(fps, 1e-3),
                            paint("geometry", ansi::gray()).c_str(),
                            out.trianglesPerFrame / 1e6);
                std::fflush(stdout);
            } else {
                std::printf("\r  %s  FPS %6.1f   frame %6.2f ms   %s   ",
                            paint("interaktiv", ansi::cyan()).c_str(), fps,
                            1000.0 / std::max(fps, 1e-3),
                            input.paused ? paint("PAUSIERT", ansi::yellow()).c_str() : "");
                std::fflush(stdout);
            }
        }
    }

    if (!opts.interactive) std::printf("\n");

    double measuredSeconds = nowSeconds() - start;
    glFinish();

    // ---- statistics -------------------------------------------------------
    out.frames = static_cast<uint64_t>(frameTimes.size());
    out.seconds = measuredSeconds;
    out.width = w.width;
    out.height = w.height;
    out.frameTimesMs = frameTimes;

    if (frameTimes.empty()) {
        error = "no frames were rendered";
        w.close();
        return false;
    }

    std::vector<double> sorted = frameTimes;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double v : frameTimes) sum += v;
    out.avgFrameMs = sum / static_cast<double>(frameTimes.size());
    out.minFps = 1000.0 / sorted.back();     // slowest frame
    out.maxFps = 1000.0 / sorted.front();    // fastest frame
    out.medianFps = 1000.0 / sorted[sorted.size() / 2];
    size_t p1 = std::min(sorted.size() - 1, static_cast<size_t>(sorted.size() * 0.99));
    out.p1Fps = 1000.0 / sorted[p1];
    out.avgFps = out.frames / measuredSeconds;
    if (out.avgFps <= 0.0 || !std::isfinite(out.avgFps))
        out.avgFps = 1000.0 / std::max(out.avgFrameMs, 1e-3);

    out.geometryRate = static_cast<double>(out.trianglesPerFrame) * out.avgFps;
    out.pixelRate = static_cast<double>(out.width) * out.height * out.avgFps;

    // Normalised score: geometry and fill throughput combined.
    constexpr double refTris = 1.5e9;      // tris/s  -> 1000 points
    constexpr double refPixels = 3.0e8;    // px/s    -> 1000 points
    double geoScore = out.geometryRate / refTris * 1000.0;
    double fillScore = out.pixelRate / refPixels * 1000.0;
    out.score = std::sqrt(std::max(geoScore, 1e-9) * std::max(fillScore, 1e-9));

    // cleanup
    glDeleteTextures(1, &hdr.color);
    glDeleteRenderbuffers(1, &hdr.depth);
    glDeleteFramebuffers(1, &hdr.fbo);
    glDeleteTextures(1, &bloomA.color);
    glDeleteFramebuffers(1, &bloomA.fbo);
    glDeleteTextures(1, &bloomB.color);
    glDeleteFramebuffers(1, &bloomB.fbo);
    glDeleteProgram(sceneProg);
    glDeleteProgram(brightProg);
    glDeleteProgram(blurProg);
    glDeleteProgram(compProg);
    glDeleteVertexArrays(1, &fullVao);
    glDeleteVertexArrays(1, &knot.vao);
    glDeleteBuffers(1, &knot.vbo);
    glDeleteBuffers(1, &knot.ebo);
    glDeleteVertexArrays(1, &floor.vao);
    glDeleteBuffers(1, &floor.vbo);
    glDeleteBuffers(1, &floor.ebo);

    w.close();
    return true;
}

} // namespace bench
