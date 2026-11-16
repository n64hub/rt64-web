//
// RT64
//

#include "rt64_render_hooks.h"

namespace RT64 {
    static RenderHookInit *init = nullptr;
    static RenderHookDraw *draw = nullptr;
    static RenderHookDeinit *deinit = nullptr;
    static RenderHookReplaceDevice *replaceDevice = nullptr;

    RenderHookInit *GetRenderHookInit() {
        return init;
    }

    RenderHookDraw *GetRenderHookDraw() {
        return draw;
    }

    RenderHookDeinit *GetRenderHookDeinit() {
        return deinit;
    }

    void SetRenderHooks(RenderHookInit *init_, RenderHookDraw *draw_, RenderHookDeinit *deinit_) {
        init = init_;
        draw = draw_;
        deinit = deinit_;
    }

    RenderHookReplaceDevice *GetRenderHookReplaceDevice() {
        return replaceDevice;
    }

    void SetRenderHookReplaceDevice(RenderHookReplaceDevice *replaceDevice_) {
        replaceDevice = replaceDevice_;
    }
};