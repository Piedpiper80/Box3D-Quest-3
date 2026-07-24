// gl_helpers.h
//
// Small OpenGL ES 3.0 helpers: shader/program compilation, a unit cube mesh,
// and error logging. Kept header-only for simplicity — this is a single-binary
// project.
#pragma once

#include <GLES3/gl3.h>
#include <android/log.h>

#define GLH_LOG_TAG "Box3DQuest"
#define GLH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, GLH_LOG_TAG, __VA_ARGS__)
#define GLH_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, GLH_LOG_TAG, __VA_ARGS__)

inline GLuint glhCompileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        GLH_LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

inline GLuint glhCreateProgram(const char* vsSrc, const char* fsSrc)
{
    GLuint vs = glhCompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = glhCompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (vs == 0 || fs == 0)
    {
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        GLH_LOGE("program link failed: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    // Shaders are retained by the program once linked; flag them for deletion.
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// A cube spanning [-1, 1] on each axis, with per-face (flat) normals. 24 unique
// vertices (4 per face) and 36 indices. Interleaved layout: position(3), normal(3).
struct CubeMesh
{
    GLuint  vao         = 0;
    GLuint  vbo         = 0;
    GLuint  ebo         = 0;
    GLsizei indexCount  = 0;
};

inline CubeMesh glhCreateCube()
{
    // clang-format off
    static const float verts[] = {
        // +X face
         1,-1,-1,  1,0,0,   1,-1, 1,  1,0,0,   1, 1, 1,  1,0,0,   1, 1,-1,  1,0,0,
        // -X face
        -1,-1, 1, -1,0,0,  -1,-1,-1, -1,0,0,  -1, 1,-1, -1,0,0,  -1, 1, 1, -1,0,0,
        // +Y face
        -1, 1,-1,  0,1,0,   1, 1,-1,  0,1,0,   1, 1, 1,  0,1,0,  -1, 1, 1,  0,1,0,
        // -Y face
        -1,-1, 1,  0,-1,0,  1,-1, 1,  0,-1,0,  1,-1,-1,  0,-1,0, -1,-1,-1,  0,-1,0,
        // +Z face
        -1,-1, 1,  0,0,1,  -1, 1, 1,  0,0,1,   1, 1, 1,  0,0,1,   1,-1, 1,  0,0,1,
        // -Z face
         1,-1,-1,  0,0,-1,  1, 1,-1,  0,0,-1, -1, 1,-1,  0,0,-1, -1,-1,-1,  0,0,-1,
    };
    static const unsigned short idx[] = {
         0, 1, 2,  0, 2, 3,
         4, 5, 6,  4, 6, 7,
         8, 9,10,  8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };
    // clang-format on

    CubeMesh mesh;
    mesh.indexCount = sizeof(idx) / sizeof(idx[0]);

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); // normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

    glBindVertexArray(0);
    return mesh;
}
