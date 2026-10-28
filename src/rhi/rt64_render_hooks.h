//
// RT64
//

#pragma once

#include "common/rt64_plume.h"

namespace RT64 {
    using RenderHookInit = void(RenderInterface *rhi, RenderDevice *device);
    using RenderHookDraw = void(RenderCommandList *list, RenderFramebuffer *swapChainFramebuffer);
    using RenderHookDeinit = void();

    // Where a lost device is replaced with a new one and RT64 makes everything again on it
    // (Application::replaceLostDevice()), this is called in place of the deinit and init hooks: with nullptrs where the
    // deinit hook would be, to destroy what was made on the old device and keep the rest, and with the new interface
    // and device where the init hook would be, to make it again on them.
    using RenderHookReplaceDevice = void(RenderInterface *rhi, RenderDevice *device);

    RenderHookInit *GetRenderHookInit();
    RenderHookDraw *GetRenderHookDraw();
    RenderHookDeinit *GetRenderHookDeinit();

    void SetRenderHooks(RenderHookInit *init, RenderHookDraw *draw, RenderHookDeinit *deinit);

    RenderHookReplaceDevice *GetRenderHookReplaceDevice();

    void SetRenderHookReplaceDevice(RenderHookReplaceDevice *replaceDevice);
};
