#include "rz_internal.h"

namespace rz {

Mat4 Mat4::identity() {
    Mat4 m{};
    m[0, 0] = 1.0f; m[1, 1] = 1.0f; m[2, 2] = 1.0f; m[3, 3] = 1.0f;
    return m;
}

Mat4 Mat4::translation(float x, float y, float z) {
    Mat4 m = identity();
    m[0, 3] = x; m[1, 3] = y; m[2, 3] = z;
    return m;
}

// Rotação em torno de x: y' = c·y − s·z, z' = s·y + c·z
Mat4 Mat4::rotationX(float s, float c) {
    Mat4 m = identity();
    m[1, 1] = c; m[1, 2] = -s;
    m[2, 1] = s; m[2, 2] = c;
    return m;
}

// Rotação em torno de y: x' = c·x + s·z, z' = −s·x + c·z
Mat4 Mat4::rotationY(float s, float c) {
    Mat4 m = identity();
    m[0, 0] = c;  m[0, 2] = s;
    m[2, 0] = -s; m[2, 2] = c;
    return m;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i, j] = a[i, 0] * b[0, j] + a[i, 1] * b[1, j]
                    + a[i, 2] * b[2, j] + a[i, 3] * b[3, j];
        }
    }
    return r;
}

} // namespace rz
