//
// RT64
//

#include "rt64_gbi_l3dex2.h"

#include "rt64_gbi_f3dex2.h"

namespace RT64 {
    namespace GBI_L3DEX2 {
        void line3D(State *state, DisplayList **dl) {
#       if RT64_DRAW_LINES
            GBI_F3DEX2::line3D(state, dl);
#       else
            assert(false);
#       endif
        }

        void setup(GBI *gbi) {
            GBI_F3DEX2::setup(gbi);

            gbi->map[L3DEX2_G_LINE3D] = &line3D;
        }
    }
};