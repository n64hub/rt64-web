//
// RT64
//

#pragma once

#include "rt64_common.h"

namespace RT64 {
    struct EmulatorConfiguration {
        struct Dither {
            bool postBlendNoise;
            bool postBlendNoiseNegative;
        };

        struct Framebuffer {
            bool renderToRAM;
            bool copyWithGPU;

            // Render to RAM copies the frame a display list drew into RDRAM when the next display list starts, or when
            // the screen next updates if the GPU has finished it by then, instead of waiting for it before the list
            // completes. A list no longer waits for the GPU: a browser takes 5-6 ms to hand a readback back, after all
            // the work queued before it. A game that reads a frame it just drew on the CPU (Majora's Mask's pictograph
            // and pause background, after flushing its task queue) reads the previous one instead, so it's for games
            // whose reads take a frame-old one (a patch of the screen, a row of depth). A word of a framebuffer the CPU
            // wrote to while its copy was pending keeps what the CPU wrote, unless the CPU wrote the value it already
            // held, which nothing can tell from a word left alone. So it's also only for games that don't put anything
            // in a framebuffer's memory right after drawing it: Donkey Kong 64 inflates its main code and data over the
            // framebuffers of its boot screens.
            bool renderToRAMLate;

            // With renderToRAMLate, the copy a list late brings only the framebuffers drawn as depth buffers into RDRAM, for
            // a game whose CPU reads its depth but never its frames (Excitebike 64's stadium lights look for the hills in
            // front of them in it). A depth buffer the list cleared as a colour image is left to the list's own depth copy
            // where that covers the same rows. In Chrome each framebuffer copied is a readback of its own, and the colour
            // ones cost a page with a large canvas most of what the copy costs it. Copies at a sync in the middle of a list
            // stay whole: RT64 reads those back itself.
            bool renderToRAMLateDepthOnly;
        };

        Dither dither;
        Framebuffer framebuffer;

        EmulatorConfiguration();
    };
};