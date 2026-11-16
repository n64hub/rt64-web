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
        };

        Dither dither;
        Framebuffer framebuffer;

        EmulatorConfiguration();
    };
};