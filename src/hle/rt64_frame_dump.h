//
// RT64
//

#pragma once

// RT64_FRAME_DUMP=<dir>, RT64_FRAME_DUMP_SECONDS=<from>-<to>: what a machine checks of a game's picture instead of an eye
// (N64Recomp-browser's tools/frame_check.mjs), off unless the variable is set, for a desktop build or a test.
//
// - <dir>/frames.bin: every frame presented, the frames interpolated between the game's included, each as its 40-byte
//   header (FrameHeader) and its pixels, 8-bit RGB, rows top down, the picture box-filtered to at most MaxWidth wide.
//   The picture is the box of the frame's color target the VI draws from (VIRenderer::render()'s videoResolution, from
//   its top left corner), not the whole target, which can be larger: the targets of the frames drawn between the
//   game's are kept at the largest size drawn (a 1692x960 target for a 1280x720 picture), and one kept from a larger
//   frame before holds that frame's picture past the box. So every frame of a picture's size comes out the same size,
//   with nothing past it. A frame flickering into a wrong picture for a frame between two right ones (a model paired with a
//   look-alike's place, a frame drawn in parts) is a frame the frames either side of it don't explain, which only
//   consecutive frames tell: a window's captures come tens of milliseconds apart, at a random point between the
//   game's frames, and a person or a model looking at one doesn't read a tree standing where another one stood.
// - <dir>/sides.tsv: for each workload the game's display lists drew (a line once the next one draws), how many of its triangles drawn through a
//   perspective projection lie in each band of the picture's width (the middle of each one's span across it), in
//   bands of the view's own width (BandsPerView a view) from one view left of it to one right, as
//   "<workload's number in the dump>\t<ms since 1970>\t<count>\t<count>...\t<past>". A game culls its world to its own 4:3 view, so with the
//   picture widened (Expand) its triangles thin out past that view's sides, which no frame shows as a fault where
//   water or sky lies under the culled ground. A triangle counts once, where it lies, not in every band it reaches
//   over: the ground's large triangles near the camera reach past both sides whatever is culled. <past> is how many
//   of the workload's model matrices (a triangle's is its first vertex's world transform) drew triangles that all lie
//   wholly past the view's left side, or all wholly past its right: a whole object drawn off the view. A game that
//   culls its objects to its 4:3 view draws next to none of them in a 4:3 window, whose picture is the view, where
//   one that culls nothing by the window draws every object near enough past it, so the count tells the two apart in
//   a 4:3 window's dump, which the bands can't: the world one draws past the view there reads alike in both. A
//   level's ground is one matrix reaching into the view, and a matrix with a triangle reaching behind the camera
//   counts as reaching it.
//
// With RT64_FRAME_DUMP_SECONDS, only what comes between those seconds after the first present is written, a scene's
// worth rather than gigabytes of the boot: about 40 MB a second of frames at 640 wide and 60 a second.
//
// The present thread copies each frame's picture into a buffer in that frame's own commands (plume's
// copyTextureRegion into a placed footprint: D3D12, and Vulkan from the plume-web commit that adds it; Metal's reads
// none back, so a desktop build's dump is for Windows and Linux), and box-filters it as the next frame is presented,
// once those commands were waited for, which costs a present a few milliseconds: the dump is a test's, not a
// player's. A frame's header is timed then, so a frame's time lags its own by one frame's.
//
// RT64 calls it from two places only, the present queue's readback() and the RSP's addTriangle(), and everything else
// is here: a game's own changes to RT64 meet its lines in as few places as can be.
//
// Frames go to a thread of their own, a few in hand at most: one that finds the queue full is dropped and counted in
// the next header's dropped field, so a slow disk shows as a gap the tool knows, never as a slower game.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/rt64_plume.h"
#include "render/rt64_vi_renderer.h"

namespace RT64 {
    struct FrameDump {
        static constexpr uint32_t Magic = 0x44465452; // "RTFD"
        static constexpr uint32_t MaxWidth = 640;
        static constexpr int BandsPerView = 20;
        static constexpr int Bands = BandsPerView * 3;
        static constexpr size_t MaxQueued = 8;

        struct FrameHeader {
            uint32_t magic;
            uint32_t width;
            uint32_t height;
            uint32_t index; // This frame's place among its present's frames.
            uint32_t count; // The frames its present showed.
            uint32_t dropped; // Frames dropped since the last one written: the writer fell behind.
            uint64_t presentId;
            uint64_t timeMs; // Milliseconds since 1970, RT64_FRAME_LOG's clock.
        };

        struct Frame {
            FrameHeader header;
            std::vector<uint8_t> rgb;
        };

        std::string dir;
        double fromSeconds = 0.0;
        double toSeconds = 1e30;
        std::chrono::steady_clock::time_point start;
        std::atomic<bool> started = false;
        FILE *framesFile = nullptr;
        FILE *sidesFile = nullptr;
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<Frame> queue;
        std::thread writer;
        uint32_t droppedSinceLast = 0;
        uint32_t bands[Bands] = {};
        std::vector<std::pair<float, float>> matrixSpans; // Each model matrix's triangles across the view, in view widths.
        const void *bandsWorkload = nullptr;
        uint64_t workloadNumber = 0;
        std::unique_ptr<RenderBuffer> readbackBuffer;
        uint64_t readbackBufferSize = 0;
        struct {
            uint64_t presentId;
            uint32_t index;
            uint32_t count;
            uint32_t width;
            uint32_t height;
            uint32_t rowPitch;
            bool sixteenBits;
        } readbackFrame = {};

        static FrameDump *get() {
            static FrameDump *dump = []() -> FrameDump * {
                const char *variable = getenv("RT64_FRAME_DUMP");
                if ((variable == nullptr) || (variable[0] == '\0')) {
                    return nullptr;
                }

                FrameDump *d = new FrameDump();
                d->dir = variable;

                // The folder is made if it isn't there, so a first run writes its dump rather than saying where it can't.
                std::error_code error;
                std::filesystem::create_directories(std::filesystem::path(d->dir), error);

                const char *seconds = getenv("RT64_FRAME_DUMP_SECONDS");
                if (seconds != nullptr) {
                    sscanf(seconds, "%lf-%lf", &d->fromSeconds, &d->toSeconds);
                }

                d->framesFile = fopen((d->dir + "/frames.bin").c_str(), "wb");
                d->sidesFile = fopen((d->dir + "/sides.tsv").c_str(), "w");
                if ((d->framesFile == nullptr) || (d->sidesFile == nullptr)) {
                    fprintf(stderr, "RT64_FRAME_DUMP: can't write to %s\n", d->dir.c_str());
                    return nullptr;
                }

                d->writer = std::thread(&FrameDump::writeLoop, d);
                d->writer.detach();
                return d;
            }();

            return dump;
        }

        static uint64_t nowMs() {
            return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // Whether now is in the window to write, counted from the first present.
        bool inWindow() {
            if (!started) {
                return false;
            }

            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return (seconds >= fromSeconds) && (seconds <= toSeconds);
        }

        // A triangle drawn through a perspective projection across view x from x0 to x1, its view's left edge at left
        // and its width width, all in the same units (RSP::drawIndexedTri()'s posScreen and viewport), with the model
        // matrix matrix (its first vertex's world transform), into the workload at workload: the last one's counts are
        // written once another one draws. An unprojectable one (a vertex behind the camera) has no place across the view,
        // and only marks its matrix as reaching it. Game thread.
        void addTriangle(const void *workload, uint32_t matrix, bool unprojectable, float x0, float x1, float left, float width) {
            if (workload != bandsWorkload) {
                if ((bandsWorkload != nullptr) && inWindow()) {
                    uint32_t past = 0;
                    for (const auto &span : matrixSpans) {
                        past += (span.first <= span.second) && ((span.second < 0.0f) || (span.first > 1.0f)) ? 1 : 0;
                    }

                    fprintf(sidesFile, "%llu\t%llu", (unsigned long long)(workloadNumber), (unsigned long long)(nowMs()));
                    for (int b = 0; b < Bands; b++) {
                        fprintf(sidesFile, "\t%u", bands[b]);
                    }

                    fprintf(sidesFile, "\t%u\n", past);
                    fflush(sidesFile);
                }

                memset(bands, 0, sizeof(bands));
                matrixSpans.clear();
                bandsWorkload = workload;
                workloadNumber++;
            }

            if (!(width > 0.0f)) {
                return;
            }

            // Unused matrices are left an empty span (its start past its end), which counts nowhere.
            if (matrix >= matrixSpans.size()) {
                matrixSpans.resize(matrix + 1, { 1e30f, -1e30f });
            }

            auto &span = matrixSpans[matrix];
            if (unprojectable) {
                span = { -1e30f, 1e30f };
                return;
            }

            span.first = std::min(span.first, (x0 - left) / width);
            span.second = std::max(span.second, (x1 - left) / width);

            const float u = ((x0 + x1) * 0.5f - left) / width;
            const int b = int(std::floor((u + 1.0f) * BandsPerView));
            if ((b >= 0) && (b < Bands)) {
                bands[b]++;
            }
        }

        // The frame a present shows, from its color target once the frame is drawn into it (the VI renderer's p, which
        // has drawn it): read back by the present's commands, and handed on at the next one's, the present that read it
        // back waited for since then. Present thread.
        void readback(const VIRenderer::RenderParams &p, uint64_t presentId, uint32_t index, uint32_t count) {
            if (readbackFrame.width > 0) {
                const uint8_t *data = reinterpret_cast<const uint8_t *>(readbackBuffer->map());
                if (data != nullptr) {
                    addFrame(readbackFrame.presentId, readbackFrame.index, readbackFrame.count, data, readbackFrame.width,
                        readbackFrame.height, readbackFrame.rowPitch, readbackFrame.sixteenBits);
                    readbackBuffer->unmap();
                }

                readbackFrame.width = 0;
            }

            const bool sixteenBits = (p.textureFormat == RenderFormat::R16G16B16A16_UNORM);
            if ((p.texture == nullptr) || (p.vi == nullptr) || ((p.textureFormat != RenderFormat::R8G8B8A8_UNORM) && !sixteenBits)) {
                return;
            }

            // The picture's box, as the VI shader reads the texture: the framebuffer's size at the target's scale.
            const uint32_t fbWidth = p.vi->fbSize().x;
            const uint32_t fbHeight = p.vi->fbSize().y;
            const float scale = float(std::max(p.downsamplingScale, 1U));
            const uint32_t width = std::min(p.textureWidth, uint32_t(std::lround(float(fbWidth) * float(p.resolutionScale.x) / scale)));
            const uint32_t height = std::min(p.textureHeight, uint32_t(std::lround(float(fbHeight) * float(p.resolutionScale.y) / scale)));
            if ((width == 0) || (height == 0)) {
                return;
            }

            const uint32_t pixelSize = sixteenBits ? 8 : 4;
            const uint32_t rowPitch = ((width * pixelSize) + 255) & ~255U;
            const uint64_t size = uint64_t(rowPitch) * height;
            if ((readbackBuffer == nullptr) || (readbackBufferSize < size)) {
                readbackBuffer = p.device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
                readbackBufferSize = size;
            }

            RenderCommandList *commandList = p.commandList;
            const RenderBox box(0, 0, int32_t(width), int32_t(height));
            commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(p.texture, RenderTextureLayout::COPY_SOURCE));
            commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(readbackBuffer.get(), RenderBufferAccess::WRITE));
            commandList->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readbackBuffer.get(), p.textureFormat, width, height, 1, rowPitch / pixelSize),
                RenderTextureCopyLocation::Subresource(p.texture), 0, 0, 0, &box);
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(p.texture, RenderTextureLayout::SHADER_READ));
            readbackFrame = { presentId, index, count, width, height, rowPitch, sixteenBits };
        }

        // A presented frame's pixels, as a readback of its color target holds them: rows rowPitch bytes apart, 4
        // channels of 8 or of 16 bits. Present thread.
        void addFrame(uint64_t presentId, uint32_t index, uint32_t count, const uint8_t *data, uint32_t width, uint32_t height,
            uint32_t rowPitch, bool sixteenBits)
        {
            if (!started.exchange(true)) {
                start = std::chrono::steady_clock::now();
            }

            if (!inWindow()) {
                return;
            }

            const uint32_t factor = (width + MaxWidth - 1) / MaxWidth;
            const uint32_t outWidth = width / factor;
            const uint32_t outHeight = height / factor;
            Frame frame;
            frame.header.magic = Magic;
            frame.header.width = outWidth;
            frame.header.height = outHeight;
            frame.header.index = index;
            frame.header.count = count;
            frame.header.presentId = presentId;
            frame.header.timeMs = nowMs();
            frame.rgb.resize(size_t(outWidth) * outHeight * 3);
            const uint32_t pixelSize = sixteenBits ? 8 : 4;
            const uint32_t area = factor * factor;
            for (uint32_t y = 0; y < outHeight; y++) {
                for (uint32_t x = 0; x < outWidth; x++) {
                    uint32_t sum[3] = {};
                    for (uint32_t fy = 0; fy < factor; fy++) {
                        const uint8_t *row = data + size_t(y * factor + fy) * rowPitch;
                        for (uint32_t fx = 0; fx < factor; fx++) {
                            const uint8_t *p = row + size_t(x * factor + fx) * pixelSize;
                            for (int c = 0; c < 3; c++) {
                                // A 16-bit channel's high byte, little endian.
                                sum[c] += sixteenBits ? p[c * 2 + 1] : p[c];
                            }
                        }
                    }

                    uint8_t *out = &frame.rgb[(size_t(y) * outWidth + x) * 3];
                    for (int c = 0; c < 3; c++) {
                        out[c] = uint8_t(sum[c] / area);
                    }
                }
            }

            std::unique_lock<std::mutex> lock(mutex);
            if (queue.size() >= MaxQueued) {
                droppedSinceLast++;
                return;
            }

            frame.header.dropped = droppedSinceLast;
            droppedSinceLast = 0;
            queue.emplace_back(std::move(frame));
            condition.notify_one();
        }

        void writeLoop() {
            for (;;) {
                Frame frame;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [&]() { return !queue.empty(); });
                    frame = std::move(queue.front());
                    queue.pop_front();
                }

                fwrite(&frame.header, sizeof(frame.header), 1, framesFile);
                fwrite(frame.rgb.data(), 1, frame.rgb.size(), framesFile);
                fflush(framesFile);
            }
        }
    };
};
