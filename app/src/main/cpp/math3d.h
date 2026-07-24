// math3d.h
//
// Minimal column-major 4x4 matrix helpers for VR rendering. Column-major means
// element m[col*4 + row], matching OpenGL's memory layout, so these matrices go
// straight into glUniformMatrix4fv with transpose = GL_FALSE.
//
// The two VR-critical functions are:
//   * m4_projection_fov  — the asymmetric per-eye projection from an OpenXR FOV,
//     for an OpenGL [-1,1] depth clip space.
//   * m4_view_from_pose  — the view matrix, i.e. the inverse of the head/eye
//     world pose reported by xrLocateViews.
#pragma once

#include <cmath>

inline void m4_identity(float* m)
{
    for (int i = 0; i < 16; ++i)
    {
        m[i] = 0.0f;
    }
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b  (column-major). Uses a temporary so out may alias a or b.
inline void m4_mul(float* out, const float* a, const float* b)
{
    float t[16];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            t[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] +
                               a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] +
                               a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    for (int i = 0; i < 16; ++i)
    {
        out[i] = t[i];
    }
}

// Asymmetric projection from an OpenXR field of view (angles in radians).
// Matches Khronos' XrMatrix4x4f_CreateProjectionFov for GRAPHICS_OPENGL:
// a right-handed projection into a [-1, 1] NDC depth range.
inline void m4_projection_fov(float* m, float angleLeft, float angleRight, float angleUp,
                              float angleDown, float nearZ, float farZ)
{
    const float tanLeft   = std::tan(angleLeft);
    const float tanRight  = std::tan(angleRight);
    const float tanDown   = std::tan(angleDown);
    const float tanUp     = std::tan(angleUp);
    const float tanWidth  = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown; // positive-Y-up (OpenGL) clip space
    const float offsetZ   = nearZ;           // nearZ for a [-1,1] depth clip space

    m[0]  = 2.0f / tanWidth;
    m[1]  = 0.0f;
    m[2]  = 0.0f;
    m[3]  = 0.0f;

    m[4]  = 0.0f;
    m[5]  = 2.0f / tanHeight;
    m[6]  = 0.0f;
    m[7]  = 0.0f;

    m[8]  = (tanRight + tanLeft) / tanWidth;
    m[9]  = (tanUp + tanDown) / tanHeight;
    m[10] = -(farZ + offsetZ) / (farZ - nearZ);
    m[11] = -1.0f;

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = -(farZ * (nearZ + offsetZ)) / (farZ - nearZ);
    m[15] = 0.0f;
}

// Rotation matrix (column-major, upper-left 3x3) from a unit quaternion (x,y,z,w).
inline void m4_from_quat(float* m, const float q[4])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    m[0]  = 1.0f - 2.0f * (yy + zz);
    m[1]  = 2.0f * (xy + wz);
    m[2]  = 2.0f * (xz - wy);
    m[3]  = 0.0f;

    m[4]  = 2.0f * (xy - wz);
    m[5]  = 1.0f - 2.0f * (xx + zz);
    m[6]  = 2.0f * (yz + wx);
    m[7]  = 0.0f;

    m[8]  = 2.0f * (xz + wy);
    m[9]  = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
    m[11] = 0.0f;

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

// View matrix = inverse of the world-space eye pose (orientation q, position p).
// For a rigid transform, the inverse rotation is the transpose and the inverse
// translation is -(R^T * p).
inline void m4_view_from_pose(float* m, const float q[4], const float p[3])
{
    float rot[16];
    m4_from_quat(rot, q);

    // Transpose the 3x3 rotation in place into the view matrix.
    m[0]  = rot[0];  m[1]  = rot[4];  m[2]  = rot[8];   m[3]  = 0.0f;
    m[4]  = rot[1];  m[5]  = rot[5];  m[6]  = rot[9];   m[7]  = 0.0f;
    m[8]  = rot[2];  m[9]  = rot[6];  m[10] = rot[10];  m[11] = 0.0f;

    // Translation = -(R^T * p).
    m[12] = -(m[0] * p[0] + m[4] * p[1] + m[8]  * p[2]);
    m[13] = -(m[1] * p[0] + m[5] * p[1] + m[9]  * p[2]);
    m[14] = -(m[2] * p[0] + m[6] * p[1] + m[10] * p[2]);
    m[15] = 1.0f;
}
