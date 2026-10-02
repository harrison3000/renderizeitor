#include "rz_internal.h"

namespace rz {

Mat4 Mat4::identity() {
    Mat4 m{};
    m[0, 0] = 1.0f; m[1, 1] = 1.0f; m[2, 2] = 1.0f; m[3, 3] = 1.0f;
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
