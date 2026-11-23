//
// RT64
//

#pragma once

// RT64_FRAME_DUMP=<dir>, RT64_FRAME_DUMP_SECONDS=<from>-<to>: what a machine checks of a game's picture instead of an
// eye, off unless the variable is set, for a desktop build or a test.
//
// - <dir>/frames.bin: every frame presented, the frames interpolated between the game's included, each as its 40-byte
//   header (FrameHeader) and its pixels, 8-bit RGB, rows top down, the picture box-filtered to at most MaxWidth wide.
//   A desktop build writes the pixels compressed (writeLoop()): the header's magic is MagicCompressed, and 8 more bytes
//   follow it (CompressedHeader), the compressed size and whether the pixels are less the previous frame's, then that
//   many bytes of zstd. A reader still reads a frame without the ones before it from the frame written whole at most
//   KeyEvery frames before it, each header saying where the next one is. A 640x360 frame is 690 KB raw, 40 MB a second at
//   60, and a dump of a whole plan was tens of GB a window: compressed it is about a third of that (Mario Golf's,
//   3.4-3.7 times smaller less the previous frame, 1.8-2.1 alone), at 1-3 ms a frame on the writer's thread. A
//   browser's build writes them raw (Magic), so its wasm carries no compressor.
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
//   "<workload's number in the dump>\t<ms since 1970>\t<count>\t<count>...". A game culls its world to its own 4:3 view, so with the
//   picture widened (Expand) its triangles thin out past that view's sides, which no frame shows as a fault where
//   water or sky lies under the culled ground. A triangle counts once, where it lies, not in every band it reaches
//   over: the ground's large triangles near the camera reach past both sides whatever is culled.
// - <dir>/draws.bin: what drew each frame, for a tool to say what covers a part of a flagged frame (the draws there,
//   what the interpolation paired each with in the game frame before, whether a cut was taken) without a debug print,
//   a rebuild and another play. A block for each game frame, its 16-byte header (DrawsHeader: magic, the size of what
//   follows, the game frame's last workload's id) then its text, zstd (DrawsMagicCompressed) on a desktop build, raw
//   (DrawsMagic) in a browser's. The text is lines of tab-separated fields, each line's first its kind:
//     F <last workload id> <paired with the frame before (GameFrame::match() ran): 1/0>: the game frame.
//     W <workload id> <the workload before it was paired with, or -> <camera cut: 1/0> <fb pairs>: each workload.
//     C <workload id> <fb pair> <projection> <type> <call index> <triangles> <view-projection> <its pair in the
//       workload before, or -> <first world transform> <last> <the first's pair, or -> <flags, or -: m moved unpaired
//       with the camera, s its vertices its pair's> <first tile's texture hash, or -> <tiles> <flags, or -: c a tile
//       copy, s synced> <the first tile paired: 1/0> <rect move, how far the frame before drew each edge from where
//       this one does, ulx,uly,lrx,lry in the game's pixels, or -> <the game's rect, ulx,uly,lrx,lry, or ->: each
//       call as GameFrame::match() paired it, type P perspective, O orthographic, R rect, F fill rect, T triangles.
//     R <rendered frame> <weight>: each frame drawn from the game frame, the game's own and each one between, in the
//       order presented: a present's frame i is the game frame's rendered frame i (presents.tsv).
//     B <workload id> <fb pair> <color image's address> <target width> <target height> <picture width> <picture height>:
//       a framebuffer pair drawn, the picture its color image at the resolution scale, the box from the target's top
//       left the VI shows (a target is kept at the largest size drawn), and
//     D <workload id> <call index> <left> <top> <right> <bottom>: where each of its calls drew, in the target's pixels
//       (frames.bin's picture is the target from its top left corner, scaled down), after its scissor, the widescreen
//       origins and aspect, and for a rect the frame's rect moves: a rect exactly, triangles the box of what of them
//       lies past the near plane in the game frame (the frames between move vertices on the GPU, so it is their
//       place in the game's own frame). Only the frames presented: a framebuffer drawn for render-to-RAM on the game's
//       thread writes none.
//   About a hundredth of frames.bin's size (1.6 MB beside 129 MB for 7 s of a race at 120 frames a second).
// - <dir>/presents.tsv: "<present id>\t<frame of the present>\t<frames it showed>\t<game frame's last workload id>\t<the
//   framebuffer shown>" for each frame put in frames.bin, the game frame whose draws.bin block says what drew it, and
//   the color image's address of the framebuffer the frame is (its B lines' draws are the ones in it), 0 for none: the
//   one the present took from the VI for the game's own frame, and the frame's interpolation target for one between,
//   which holds that framebuffer drawn again. A frame may draw several framebuffers and show any one of them, not the
//   largest: Mario Golf's credits fill one the size of the window and draw the picture and its text into another, 226
//   rows high.
//
// With RT64_FRAME_DUMP_SECONDS, only what comes between those seconds after the first present is written, a scene's
// worth rather than gigabytes of the boot: about 40 MB a second of frames at 640 wide and 60 a second.
//
// With RT64_FRAME_DUMP_RATE=<Hz> too, RT64 presents at that rate whatever the display's is: the swap chain's vsync is
// turned off (swapChainCreated()) and its rate taken as that one (swapChainRate()), so Framerate Display interpolates
// to it. A desktop build presents frames between the game's only where the display is faster than the game, so a game
// of 60 on a 59-60 Hz display presents none, and a dump of it tests nothing of how they pair: at 120 every other
// frame is one of them. Where the device can't turn vsync off (Vulkan without its immediate present mode), the
// presents stay held to the display's.
//
// The present thread copies each frame's picture into a buffer in that frame's own commands (plume's
// copyTextureRegion into a placed footprint: D3D12, and Vulkan from the plume-web commit that adds it; Metal's reads
// none back, so a desktop build's dump is for Windows and Linux), and box-filters it as the next frame is presented,
// once those commands were waited for, which costs a present a few milliseconds: the dump is a test's, not a
// player's. A frame's header is timed then, so a frame's time lags its own by one frame's.
//
// RT64 calls it from few places, the present queue's readback(), the RSP's addTriangle(), the application's
// swapChainCreated(), the shared resources' swapChainRate(), the workload queue's addGameFrame() and
// addRenderedFrame() and the framebuffer renderer's addDraw(), and everything else is here and in rt64_frame_dump.cpp:
// a game's own changes to RT64 meet its lines in as few places as can be.
//
// Frames go to a thread of their own, about a second's in hand at most (MaxQueued, 44 MB of 640x360 frames: a dump of
// Mario Golf's lost 22 frames at once to a stall of the writer, with 8 in hand, in one of three windows that dumped
// the same scene): one that finds the queue full is dropped and counted in
// the next header's dropped field, so a slow disk shows as a gap the tool knows, never as a slower game. The writer
// stops writing frames, and says so once, when its disk has less than MinFreeBytes free: a dump run by hand, with no
// tool watching its size, never fills the disk under the game and whatever else uses it.

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
    struct GameCall;
    struct GameFrame;
    struct Projection;
    struct Workload;
    struct WorkloadQueue;

    // rt64_frame_dump.cpp: src as zstd at level into dst, sized to fit, a context kept in context between calls. False,
    // and why in error, where it fails. The one part of the dump not in this header: zstd's header is RT64's own, and
    // programs include this one without it (RecompFrontend's renderer).
    bool frameDumpCompress(void *&context, const std::vector<uint8_t> &src, std::vector<uint8_t> &dst, int level, const char *&error);

    struct FrameDump {
        static constexpr uint32_t Magic = 0x44465452; // "RTFD": the pixels follow raw.
        static constexpr uint32_t MagicCompressed = 0x5A465452; // "RTFZ": a CompressedHeader and zstd follow.
        static constexpr uint32_t KeyEvery = 30; // A frame written whole at least this often.
        static constexpr int CompressionLevel = 3;
        static constexpr uint64_t MinFreeBytes = 10ULL << 30;
        static constexpr uint32_t MaxWidth = 640;
        static constexpr int BandsPerView = 20;
        static constexpr int Bands = BandsPerView * 3;
        static constexpr size_t MaxQueued = 64;
        static constexpr uint32_t DrawsMagic = 0x57445452; // "RTDW": draws.bin's text follows raw.
        static constexpr uint32_t DrawsMagicCompressed = 0x5A445452; // "RTDZ": as zstd.
        static constexpr size_t MaxDrawsQueued = 256;

        struct DrawsHeader {
            uint32_t magic;
            uint32_t bytes; // What follows.
            uint64_t workloadId; // The game frame's last workload's.
        };

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

        struct CompressedHeader {
            uint32_t bytes; // The zstd's size, which follows.
            uint32_t previous; // 1: what it decompresses to is each byte of the pixels less the previous frame's, mod 256.
        };

        struct Frame {
            FrameHeader header;
            std::vector<uint8_t> rgb;
        };

        std::string dir;
        double fromSeconds = 0.0;
        double toSeconds = 1e30;
        uint32_t rate = 0;
        std::chrono::steady_clock::time_point start;
        std::atomic<bool> started = false;
        FILE *framesFile = nullptr;
        FILE *sidesFile = nullptr;
        FILE *drawsFile = nullptr;
        FILE *presentsFile = nullptr;
        std::deque<std::pair<uint64_t, std::string>> drawsQueue; // Game frames' text by their last workload's id.
        void *drawsCompressContext = nullptr; // Writer thread's.
        std::vector<uint8_t> drawsSource;
        std::vector<uint8_t> drawsCompressed;
        std::string drawsText; // The game frame being drawn's, and the rest, the workload thread's.
        uint64_t drawsWorkloadId = 0;
        bool drawsOn = false;
        uint32_t drawsRendered = 0;
        const void *drawsFbWorkload = nullptr;
        uint32_t drawsFbPairIndex = 0;
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<Frame> queue;
        std::thread writer;
        bool outOfRoom = false; // Writer thread's, as are the rest.
        uint64_t framesWritten = 0;
        uint32_t sinceKey = 0;
        std::vector<uint8_t> previousRgb;
        std::vector<uint8_t> difference;
        std::vector<uint8_t> compressed;
        void *compressContext = nullptr;
        uint32_t droppedSinceLast = 0;
        uint32_t bands[Bands] = {};
        const void *bandsWorkload = nullptr;
        uint64_t workloadNumber = 0;
        std::unique_ptr<RenderBuffer> readbackBuffer;
        uint64_t readbackBufferSize = 0;
        // readbackFrame's color image address of the framebuffer it is (readback()'s shownAddress), which addFrame()
        // writes in its presents.tsv line.
        uint32_t readbackShownAddress = 0;
        struct {
            uint64_t presentId;
            uint32_t index;
            uint32_t count;
            uint32_t width;
            uint32_t height;
            uint32_t rowPitch;
            bool sixteenBits;
            uint64_t workloadId;
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

                const char *rate = getenv("RT64_FRAME_DUMP_RATE");
                if (rate != nullptr) {
                    d->rate = uint32_t(std::max(atoi(rate), 0));
                }

                d->framesFile = fopen((d->dir + "/frames.bin").c_str(), "wb");
                d->sidesFile = fopen((d->dir + "/sides.tsv").c_str(), "w");
                d->drawsFile = fopen((d->dir + "/draws.bin").c_str(), "wb");
                d->presentsFile = fopen((d->dir + "/presents.tsv").c_str(), "w");
                if ((d->framesFile == nullptr) || (d->sidesFile == nullptr) || (d->drawsFile == nullptr) || (d->presentsFile == nullptr)) {
                    fprintf(stderr, "RT64_FRAME_DUMP: can't write to %s\n", d->dir.c_str());
                    return nullptr;
                }

                d->writer = std::thread(&FrameDump::writeLoop, d);
                d->writer.detach();
                return d;
            }();

            return dump;
        }

        // RT64_FRAME_DUMP_RATE: the rate the swap chain is taken to present at, in place of the display's rate.
        static uint32_t swapChainRate(uint32_t displayRate) {
            const FrameDump *dump = get();
            return ((dump != nullptr) && (dump->rate > 0)) ? dump->rate : displayRate;
        }

        // RT64_FRAME_DUMP_RATE: the swap chain's presents aren't held to the display's. Vulkan's present mode changes
        // at its next resize(), which the present queue makes as soon as needsResize() says so.
        static void swapChainCreated(RenderSwapChain *swapChain) {
            const FrameDump *dump = get();
            if ((dump != nullptr) && (dump->rate > 0)) {
                swapChain->setVsyncEnabled(false);
            }
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

        // The frame a present shows, from its color target once the frame is drawn into it (the VI renderer's p, which
        // has drawn it): read back by the present's commands, and handed on at the next one's, the present that read it
        // back waited for since then. workloadId is the last workload's of the game frame it was drawn from, its
        // InterpolatedFrameCounters', and shownAddress the color image's address of the framebuffer the frame is, for
        // presents.tsv. Present thread.
        void readback(const VIRenderer::RenderParams &p, uint64_t presentId, uint32_t index, uint32_t count, uint64_t workloadId,
            uint32_t shownAddress)
        {
            if (readbackFrame.width > 0) {
                const uint8_t *data = reinterpret_cast<const uint8_t *>(readbackBuffer->map());
                if (data != nullptr) {
                    addFrame(readbackFrame.presentId, readbackFrame.index, readbackFrame.count, data, readbackFrame.width,
                        readbackFrame.height, readbackFrame.rowPitch, readbackFrame.sixteenBits, readbackFrame.workloadId);
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

            readbackShownAddress = shownAddress;
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
            readbackFrame = { presentId, index, count, width, height, rowPitch, sixteenBits, workloadId };
        }

        // A presented frame's pixels, as a readback of its color target holds them: rows rowPitch bytes apart, 4
        // channels of 8 or of 16 bits. Present thread.
        void addFrame(uint64_t presentId, uint32_t index, uint32_t count, const uint8_t *data, uint32_t width, uint32_t height,
            uint32_t rowPitch, bool sixteenBits, uint64_t workloadId)
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
            fprintf(presentsFile, "%llu\t%u\t%u\t%llu\t%08x\n", (unsigned long long)(presentId), index, count,
                (unsigned long long)(workloadId), readbackShownAddress);
            fflush(presentsFile);
        }

        // draws.bin (rt64_frame_dump.cpp), the workload thread's: a game frame once GameFrame::match() paired it with the
        // frame before (the last one's text goes to the writer then), each frame drawn from it, and each call as the
        // framebuffer renderer places it: its type as C's, its scissor in the target's pixels (a fill rect's rect), the
        // screen scale and offset its vertices are drawn with (InstanceDrawCall's triangles), the target's size, the
        // resolution scale and the color image's size in the game's pixels, and the widescreen aspect.
        void addGameFrame(const WorkloadQueue &workloadQueue, const GameFrame &curFrame);
        void addRenderedFrame(float weight);
        void addDraw(const Workload &workload, uint32_t fbPairIndex, const Projection &proj, const GameCall &call, char type,
            int32_t left, int32_t top, int32_t right, int32_t bottom, float scaleX, float scaleY, float offsetX, float offsetY,
            float targetWidth, float targetHeight, float resolutionScaleX, float resolutionScaleY, uint32_t colorWidth, uint32_t colorHeight,
            float aspectRatioScale);

        void writeLoop() {
            for (;;) {
                Frame frame;
                std::pair<uint64_t, std::string> draws;
                bool haveFrame = false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [&]() { return !queue.empty() || !drawsQueue.empty(); });
                    if (!drawsQueue.empty()) {
                        draws = std::move(drawsQueue.front());
                        drawsQueue.pop_front();
                    }
                    else {
                        frame = std::move(queue.front());
                        queue.pop_front();
                        haveFrame = true;
                    }
                }

                if (!haveFrame) {
                    if (!outOfRoom) {
                        writeDraws(draws.first, draws.second);
                    }

                    continue;
                }

                if (!roomToWrite()) {
                    continue;
                }

#           if defined(__EMSCRIPTEN__)
                fwrite(&frame.header, sizeof(frame.header), 1, framesFile);
                fwrite(frame.rgb.data(), 1, frame.rgb.size(), framesFile);
#           else
                writeCompressed(frame);
#           endif
                fflush(framesFile);
            }
        }

        // Whether the dump's disk has room: read every 64 frames, and once it hasn't, never again. Writer thread.
        bool roomToWrite() {
            if (outOfRoom) {
                return false;
            }

            if ((framesWritten++ % 64) == 0) {
                std::error_code error;
                const std::filesystem::space_info space = std::filesystem::space(std::filesystem::path(dir), error);
                if (!error && (space.available < MinFreeBytes)) {
                    fprintf(stderr, "RT64_FRAME_DUMP: stopped writing frames, %s has under %llu GB free\n", dir.c_str(),
                        (unsigned long long)(MinFreeBytes >> 30));
                    outOfRoom = true;
                    return false;
                }
            }

            return true;
        }

        // A game frame's draws.bin block: zstd on a desktop build, raw in a browser's. Writer thread.
        void writeDraws(uint64_t workloadId, const std::string &text) {
            DrawsHeader header = { DrawsMagic, uint32_t(text.size()), workloadId };
            const void *data = text.data();
#       if !defined(__EMSCRIPTEN__)
            drawsSource.assign(text.begin(), text.end());
            const char *error = nullptr;
            if (!frameDumpCompress(drawsCompressContext, drawsSource, drawsCompressed, CompressionLevel, error)) {
                fprintf(stderr, "RT64_FRAME_DUMP: a frame's draws didn't compress: %s\n", error);
                return;
            }

            header = { DrawsMagicCompressed, uint32_t(drawsCompressed.size()), workloadId };
            data = drawsCompressed.data();
#       endif
            fwrite(&header, sizeof(header), 1, drawsFile);
            fwrite(data, 1, header.bytes, drawsFile);
            fflush(drawsFile);
        }

#   if !defined(__EMSCRIPTEN__)
        // A frame as MagicCompressed's header and zstd: whole, at the dump's first frame, a size change or KeyEvery
        // frames since the last whole one, and otherwise each byte less the previous frame's, which leaves the parts
        // that didn't change zeros (most of a frame between two of the game's, and every still part of a scene).
        // Writer thread.
        void writeCompressed(Frame &frame) {
            const bool previous = (sinceKey < KeyEvery) && (previousRgb.size() == frame.rgb.size());
            const std::vector<uint8_t> *source = &frame.rgb;
            if (previous) {
                difference.resize(frame.rgb.size());
                for (size_t i = 0; i < frame.rgb.size(); i++) {
                    difference[i] = uint8_t(frame.rgb[i] - previousRgb[i]);
                }

                source = &difference;
                sinceKey++;
            }
            else {
                sinceKey = 1;
            }

            const char *error = nullptr;
            if (!frameDumpCompress(compressContext, *source, compressed, CompressionLevel, error)) {
                fprintf(stderr, "RT64_FRAME_DUMP: a frame didn't compress: %s\n", error);
                return;
            }

            FrameHeader header = frame.header;
            header.magic = MagicCompressed;
            const CompressedHeader extra = { uint32_t(compressed.size()), previous ? 1U : 0U };
            fwrite(&header, sizeof(header), 1, framesFile);
            fwrite(&extra, sizeof(extra), 1, framesFile);
            fwrite(compressed.data(), 1, compressed.size(), framesFile);
            previousRgb.swap(frame.rgb);
        }
#   endif
    };
};
