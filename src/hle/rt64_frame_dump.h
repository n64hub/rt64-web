//
// RT64
//

#pragma once

// RT64_FRAME_DUMP=<dir>, RT64_FRAME_DUMP_SECONDS=<from>-<to>: what a machine checks of a game's picture instead of an eye
// (N64Recomp-browser's tools/frame_check.mjs), off unless the variable is set, for a desktop build or a test.
//
// - <dir>/frames.bin: every frame presented, the frames interpolated between the game's included, each as its 40-byte
//   header (FrameHeader) and its pixels, 8-bit RGB, rows top down, the frame's color target box-filtered to at most
//   MaxWidth wide. A frame flickering into a wrong picture for a frame between two right ones (a model paired with a
//   look-alike's place, a frame drawn in parts) is a frame the frames either side of it don't explain, which only
//   consecutive frames tell: a window's captures come tens of milliseconds apart, at a random point between the
//   game's frames, and a person or a model looking at one doesn't read a tree standing where another one stood.
// - <dir>/sides.tsv: for each workload the game's display lists drew (a line once the next one draws), how many of its triangles drawn through a
//   perspective projection lie in each band of the picture's width (the middle of each one's span across it), in
//   bands of the view's own width (BandsPerView a view) from one view left of it to one right, as
//   "<workload's number in the dump>\t<ms since 1970>\t<count>\t<count>...". A game culls its world to its own 4:3 view, so with the
//   picture widened (Expand) its triangles thin out past that view's sides, which no frame shows as a fault where
//   water or sky lies under the culled ground. A triangle counts once, where it lies, not in every band it reaches
//   over: the ground's large triangles near the camera reach past both sides whatever is culled.
//
// With RT64_FRAME_DUMP_SECONDS, only what comes between those seconds after the first present is written, a scene's
// worth rather than gigabytes of the boot: about 40 MB a second of frames at 640 wide and 60 a second.
//
// The present thread copies each frame's color target into a buffer in that frame's own commands (plume's
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
        // and its width width, all in the same units (RSP::drawIndexedTri()'s posScreen and viewport), into the
        // workload at workload: the last one's counts are written once another one draws. Game thread.
        void addTriangle(const void *workload, float x0, float x1, float left, float width) {
            if (workload != bandsWorkload) {
                if ((bandsWorkload != nullptr) && inWindow()) {
                    fprintf(sidesFile, "%llu\t%llu", (unsigned long long)(workloadNumber), (unsigned long long)(nowMs()));
                    for (int b = 0; b < Bands; b++) {
                        fprintf(sidesFile, "\t%u", bands[b]);
                    }

                    fprintf(sidesFile, "\n");
                    fflush(sidesFile);
                }

                memset(bands, 0, sizeof(bands));
                bandsWorkload = workload;
                workloadNumber++;
            }

            if (!(width > 0.0f)) {
                return;
            }

            const float u = ((x0 + x1) * 0.5f - left) / width;
            const int b = int(std::floor((u + 1.0f) * BandsPerView));
            if ((b >= 0) && (b < Bands)) {
                bands[b]++;
            }
        }

        // The frame a present shows, from its color target once the frame is drawn into it: read back by the
        // present's commands, and handed on at the next one's, the present that read it back waited for since then.
        // Present thread.
        void readback(RenderDevice *device, RenderCommandList *commandList, RenderTexture *texture, RenderFormat format, uint32_t width,
            uint32_t height, uint64_t presentId, uint32_t index, uint32_t count)
        {
            if (readbackFrame.width > 0) {
                const uint8_t *data = reinterpret_cast<const uint8_t *>(readbackBuffer->map());
                if (data != nullptr) {
                    addFrame(readbackFrame.presentId, readbackFrame.index, readbackFrame.count, data, readbackFrame.width,
                        readbackFrame.height, readbackFrame.rowPitch, readbackFrame.sixteenBits);
                    readbackBuffer->unmap();
                }

                readbackFrame.width = 0;
            }

            const bool sixteenBits = (format == RenderFormat::R16G16B16A16_UNORM);
            if ((texture == nullptr) || ((format != RenderFormat::R8G8B8A8_UNORM) && !sixteenBits)) {
                return;
            }

            const uint32_t pixelSize = sixteenBits ? 8 : 4;
            const uint32_t rowPitch = ((width * pixelSize) + 255) & ~255U;
            const uint64_t size = uint64_t(rowPitch) * height;
            if ((readbackBuffer == nullptr) || (readbackBufferSize < size)) {
                readbackBuffer = device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
                readbackBufferSize = size;
            }

            commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
            commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(readbackBuffer.get(), RenderBufferAccess::WRITE));
            commandList->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readbackBuffer.get(), format, width, height, 1, rowPitch / pixelSize),
                RenderTextureCopyLocation::Subresource(texture));
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
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
