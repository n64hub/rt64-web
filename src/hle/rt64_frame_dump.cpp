//
// RT64
//

#include "rt64_frame_dump.h"

#if !defined(__EMSCRIPTEN__)
#   include <zstd.h>
#endif

namespace RT64 {
    // RT64_FRAME_DUMP's compression (rt64_frame_dump.h, FrameDump::writeCompressed()): here, where zstd's header is,
    // and a browser's build, which writes its frames raw, links no compressor.
    bool frameDumpCompress(void *&context, const std::vector<uint8_t> &src, std::vector<uint8_t> &dst, int level, const char *&error) {
#   if defined(__EMSCRIPTEN__)
        error = "a browser's build doesn't compress";
        return false;
#   else
        if (context == nullptr) {
            context = ZSTD_createCCtx();
        }

        dst.resize(ZSTD_compressBound(src.size()));
        const size_t bytes = ZSTD_compressCCtx(static_cast<ZSTD_CCtx *>(context), dst.data(), dst.size(), src.data(), src.size(), level);
        if (ZSTD_isError(bytes)) {
            error = ZSTD_getErrorName(bytes);
            return false;
        }

        dst.resize(bytes);
        return true;
#   endif
    }
};
