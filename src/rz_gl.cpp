#include "rz_gl.h"

namespace rz {

#define RZ_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
RZ_GL_FUNCTIONS(RZ_GL_DEFINE)
#undef RZ_GL_DEFINE

bool loadGl(GetProcFn getProc) {
    bool ok = true;
#define RZ_GL_LOAD(ret, name, args)                                   \
    name = reinterpret_cast<PFN_##name>(getProc(#name));             \
    if (!name) ok = false;
    RZ_GL_FUNCTIONS(RZ_GL_LOAD)
#undef RZ_GL_LOAD
    return ok;
}

} // namespace rz
