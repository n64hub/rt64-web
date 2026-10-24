//
// RT64
//

#include "rt64_inspector.h"

#include <algorithm>
#include <cstddef>
#include <iomanip>

#include "im3d/im3d.h"
#include "im3d/im3d_math.h"
#include "imgui/imgui.h"
#include "implot/implot.h"

// Volk must be included before the ImGui Vulkan backend.
#include "plume_vulkan.h"

#include "imgui/imgui_impl_sdl2_custom.h"
#include "imgui/backends/imgui_impl_vulkan.h"

#if defined(_WIN32)
#   include "imgui/backends/imgui_impl_dx12.h"
#   include "imgui/backends/imgui_impl_win32.h"
#   include "utf8conv/utf8conv.h"
#endif

#if defined(_WIN32)
#   include "plume_d3d12.h"
#endif

static std::string IniFilenameUTF8;

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

static void checkVulkanResult(VkResult res) {
    if (res == 0) {
        return;
    }

    fprintf(stderr, "ImGui Vulkan Backend failed with error code 0x%X.\n", res);
}

namespace RT64 {
    struct VulkanContext {
        VkDevice device = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

        ~VulkanContext() {
#       if defined(__EMSCRIPTEN__)
            // A page has only WebGPU, so there is no Vulkan inspector to clean up.
#       else
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device, renderPass, nullptr);
            }

            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            }
#       endif
        }
    };

#if defined(__EMSCRIPTEN__)
    // A page has only WebGPU, and its device belongs to the page's main thread (plume_device_thread.h), so ImGui's own
    // WebGPU backend, which calls WebGPU on whatever thread renders, can't draw there. The inspector draws ImGui's lists
    // through plume instead, as RT64 draws everything else: the font atlas is a texture, each frame's vertices and
    // indices go in upload buffers, and one WGSL pipeline draws each command clipped to its rectangle. The font atlas
    // is the one texture it has, so a command drawing any other ImTextureID (a desktop backend's handle) is skipped.
    static const char WebGPUInspectorShader[] = R"(
struct PushConstants {
    scale: vec2<f32>,
    translate: vec2<f32>,
};

@group(0) @binding(0) var fontTexture: texture_2d<f32>;
@group(0) @binding(1) var fontSampler: sampler;
@group(3) @binding(999) var<uniform> pushConstants: PushConstants;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn VSMain(@location(0) position: vec2<f32>, @location(1) uv: vec2<f32>, @location(2) color: vec4<f32>) -> VertexOutput {
    var output: VertexOutput;
    output.position = vec4<f32>(position * pushConstants.scale + pushConstants.translate, 0.0, 1.0);
    output.color = color;
    output.uv = uv;
    return output;
}

@fragment
fn PSMain(input: VertexOutput) -> @location(0) vec4<f32> {
    return input.color * textureSample(fontTexture, fontSampler, input.uv);
}
)";

    struct Inspector::WebGPUContext {
        RenderDevice *device = nullptr;
        RenderDescriptorSetBuilder descriptorSetBuilder;
        std::unique_ptr<RenderSampler> sampler;
        std::unique_ptr<RenderPipelineLayout> pipelineLayout;
        std::unique_ptr<RenderPipeline> pipeline;
        std::unique_ptr<RenderTexture> fontTexture;
        std::unique_ptr<RenderDescriptorSet> fontDescriptorSet;
        std::unique_ptr<RenderBuffer> vertexBuffer;
        std::unique_ptr<RenderBuffer> indexBuffer;
        uint64_t vertexBufferSize = 0;
        uint64_t indexBufferSize = 0;

        WebGPUContext(RenderDevice *device) {
            this->device = device;

            RenderSamplerDesc samplerDesc;
            samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
            samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
            samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
            sampler = device->createSampler(samplerDesc);

            descriptorSetBuilder.begin();
            descriptorSetBuilder.addTexture(0);
            descriptorSetBuilder.addSampler(1);
            descriptorSetBuilder.end();

            RenderPipelineLayoutBuilder layoutBuilder;
            layoutBuilder.begin(false, true);
            layoutBuilder.addPushConstant(0, 0, sizeof(float) * 4, RenderShaderStageFlag::VERTEX);
            layoutBuilder.addDescriptorSet(descriptorSetBuilder);
            layoutBuilder.end();
            pipelineLayout = layoutBuilder.create(device);

            std::unique_ptr<RenderShader> vertexShader = device->createShader(WebGPUInspectorShader, sizeof(WebGPUInspectorShader), "VSMain", RenderShaderFormat::WGSL);
            std::unique_ptr<RenderShader> pixelShader = device->createShader(WebGPUInspectorShader, sizeof(WebGPUInspectorShader), "PSMain", RenderShaderFormat::WGSL);
            const RenderInputSlot inputSlot(0, sizeof(ImDrawVert));
            const RenderInputElement inputElements[] = {
                RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, pos)),
                RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, uv)),
                RenderInputElement("COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col))
            };

            RenderGraphicsPipelineDesc pipelineDesc;
            pipelineDesc.pipelineLayout = pipelineLayout.get();
            pipelineDesc.vertexShader = vertexShader.get();
            pipelineDesc.pixelShader = pixelShader.get();
            pipelineDesc.renderTargetFormat[0] = device->getCapabilities().preferredSwapChainFormat;
            pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
            pipelineDesc.renderTargetCount = 1;
            pipelineDesc.inputSlots = &inputSlot;
            pipelineDesc.inputSlotsCount = 1;
            pipelineDesc.inputElements = inputElements;
            pipelineDesc.inputElementsCount = uint32_t(std::size(inputElements));
            pipeline = device->createGraphicsPipeline(pipelineDesc);
        }

        // Makes the font atlas's texture once ImGui has built the atlas, and again whenever the program rebuilds it (a
        // font added, a new size), as the desktop backends' NewFrame() does.
        void updateFontTexture() {
            ImFontAtlas *fonts = ImGui::GetIO().Fonts;
            if ((fontDescriptorSet != nullptr) && fonts->IsBuilt() && (fonts->TexID == ImTextureID(fontDescriptorSet.get()))) {
                return;
            }

            unsigned char *pixels = nullptr;
            int width = 0, height = 0;
            fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            const uint32_t rowPitch = uint32_t(width) * 4;
            const uint32_t alignedRowPitch = (rowPitch + 255) & ~255U;
            std::unique_ptr<RenderBuffer> uploadBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(alignedRowPitch) * height));
            uint8_t *uploadData = reinterpret_cast<uint8_t *>(uploadBuffer->map());
            for (int y = 0; y < height; y++) {
                memcpy(uploadData + uint64_t(alignedRowPitch) * y, pixels + uint64_t(rowPitch) * y, rowPitch);
            }

            uploadBuffer->unmap();

            fontTexture = device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM));
            {
                RenderWorker worker(device, "Inspector font", RenderCommandListType::DIRECT);
                RenderWorkerExecution execution(&worker);
                worker.commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(fontTexture.get(), RenderTextureLayout::COPY_DEST));
                worker.commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(fontTexture.get()), RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, alignedRowPitch / 4));
                worker.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(fontTexture.get(), RenderTextureLayout::SHADER_READ));
            }

            fontDescriptorSet = descriptorSetBuilder.create(device);
            fontDescriptorSet->setTexture(0, fontTexture.get(), RenderTextureLayout::SHADER_READ);
            fontDescriptorSet->setSampler(1, sampler.get());
            fonts->SetTexID(ImTextureID(fontDescriptorSet.get()));
        }

        void setRenderState(RenderCommandList *commandList, const ImDrawData *drawData, uint32_t width, uint32_t height, uint32_t vertexBytes, uint32_t indexBytes) {
            const float pushConstants[4] = {
                2.0f / drawData->DisplaySize.x,
                -2.0f / drawData->DisplaySize.y,
                -1.0f - drawData->DisplayPos.x * 2.0f / drawData->DisplaySize.x,
                1.0f + drawData->DisplayPos.y * 2.0f / drawData->DisplaySize.y
            };

            const RenderVertexBufferView vertexBufferView(vertexBuffer.get(), vertexBytes);
            const RenderInputSlot inputSlot(0, sizeof(ImDrawVert));
            const RenderIndexBufferView indexBufferView(indexBuffer.get(), indexBytes, (sizeof(ImDrawIdx) == 2) ? RenderFormat::R16_UINT : RenderFormat::R32_UINT);
            const RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
            commandList->setViewports(&viewport, 1);
            commandList->setPipeline(pipeline.get());
            commandList->setGraphicsPipelineLayout(pipelineLayout.get());
            commandList->setGraphicsPushConstants(0, pushConstants);
            commandList->setGraphicsDescriptorSet(fontDescriptorSet.get(), 0);
            commandList->setVertexBuffers(0, &vertexBufferView, 1, &inputSlot);
            commandList->setIndexBuffer(&indexBufferView);
        }

        // Draws over the swap chain's framebuffer, which the present has set, with ImGui's display mapped onto all of
        // it. The present waits for its list before it records the next (RT64's PresentQueue), so one pair of upload
        // buffers does for every frame.
        void draw(RenderCommandList *commandList, ImDrawData *drawData, uint32_t width, uint32_t height) {
            if ((fontDescriptorSet == nullptr) || (drawData->TotalVtxCount <= 0) || (drawData->DisplaySize.x <= 0.0f) || (drawData->DisplaySize.y <= 0.0f) || (width == 0) || (height == 0)) {
                return;
            }

            // WebGPU writes buffers 4 bytes at a time.
            const uint32_t vertexBytes = (uint32_t(drawData->TotalVtxCount * sizeof(ImDrawVert)) + 3) & ~3U;
            const uint32_t indexBytes = (uint32_t(drawData->TotalIdxCount * sizeof(ImDrawIdx)) + 3) & ~3U;
            if (vertexBufferSize < vertexBytes) {
                vertexBufferSize = vertexBytes * 2;
                vertexBuffer = device->createBuffer(RenderBufferDesc::VertexBuffer(vertexBufferSize, RenderHeapType::UPLOAD));
            }

            if (indexBufferSize < indexBytes) {
                indexBufferSize = indexBytes * 2;
                indexBuffer = device->createBuffer(RenderBufferDesc::IndexBuffer(indexBufferSize, RenderHeapType::UPLOAD));
            }

            ImDrawVert *vertexData = reinterpret_cast<ImDrawVert *>(vertexBuffer->map());
            ImDrawIdx *indexData = reinterpret_cast<ImDrawIdx *>(indexBuffer->map());
            for (int i = 0; i < drawData->CmdListsCount; i++) {
                const ImDrawList *drawList = drawData->CmdLists[i];
                memcpy(vertexData, drawList->VtxBuffer.Data, drawList->VtxBuffer.Size * sizeof(ImDrawVert));
                memcpy(indexData, drawList->IdxBuffer.Data, drawList->IdxBuffer.Size * sizeof(ImDrawIdx));
                vertexData += drawList->VtxBuffer.Size;
                indexData += drawList->IdxBuffer.Size;
            }

            vertexBuffer->unmap();
            indexBuffer->unmap();

            setRenderState(commandList, drawData, width, height, vertexBytes, indexBytes);

            const float scaleX = float(width) / drawData->DisplaySize.x;
            const float scaleY = float(height) / drawData->DisplaySize.y;
            const ImTextureID fontTextureId = ImTextureID(fontDescriptorSet.get());
            int32_t vertexOffset = 0;
            uint32_t indexOffset = 0;
            for (int i = 0; i < drawData->CmdListsCount; i++) {
                const ImDrawList *drawList = drawData->CmdLists[i];
                for (const ImDrawCmd &drawCmd : drawList->CmdBuffer) {
                    if (drawCmd.UserCallback != nullptr) {
                        if (drawCmd.UserCallback == ImDrawCallback_ResetRenderState) {
                            setRenderState(commandList, drawData, width, height, vertexBytes, indexBytes);
                        }
                        else {
                            drawCmd.UserCallback(drawList, &drawCmd);
                        }

                        continue;
                    }

                    if (drawCmd.GetTexID() != fontTextureId) {
                        continue;
                    }

                    const int32_t left = std::max(int32_t((drawCmd.ClipRect.x - drawData->DisplayPos.x) * scaleX), 0);
                    const int32_t top = std::max(int32_t((drawCmd.ClipRect.y - drawData->DisplayPos.y) * scaleY), 0);
                    const int32_t right = std::min(int32_t((drawCmd.ClipRect.z - drawData->DisplayPos.x) * scaleX), int32_t(width));
                    const int32_t bottom = std::min(int32_t((drawCmd.ClipRect.w - drawData->DisplayPos.y) * scaleY), int32_t(height));
                    if ((left >= right) || (top >= bottom)) {
                        continue;
                    }

                    const RenderRect scissor(left, top, right, bottom);
                    commandList->setScissors(&scissor, 1);
                    commandList->drawIndexedInstanced(drawCmd.ElemCount, 1, indexOffset + drawCmd.IdxOffset, vertexOffset + int32_t(drawCmd.VtxOffset), 0);
                }

                vertexOffset += drawList->VtxBuffer.Size;
                indexOffset += drawList->IdxBuffer.Size;
            }
        }
    };
#endif

    // Inspector

    Inspector::Inspector(RenderDevice *device, const RenderSwapChain *swapChain, UserConfiguration::GraphicsAPI graphicsAPI, SDL_Window *sdlWindow) {
        assert(device != nullptr);
        assert(swapChain != nullptr);

        this->device = device;
        this->swapChain = swapChain;
        this->graphicsAPI = graphicsAPI;
        this->sdlWindow = sdlWindow;

        IMGUI_CHECKVERSION();

        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGui::StyleColorsDark();

        if (sdlWindow != nullptr) {
            ImGui_ImplSDL2_InitForOther(sdlWindow);
        }
        else {
#       ifdef _WIN32
            RenderWindow renderWindow = swapChain->getWindow();
            ImGui_ImplWin32_Init(renderWindow);
#       endif
        }

#   if defined(__EMSCRIPTEN__)
        // Whatever graphicsAPI says, a page draws through WebGPU (RT64::Application).
        ImGui::GetIO().BackendRendererName = "rt64_plume_webgpu";
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        webgpuContext = std::make_unique<WebGPUContext>(device);
        return;
#   endif

        switch (graphicsAPI) {
        case UserConfiguration::GraphicsAPI::D3D12: {
#       ifdef _WIN32
            D3D12Device *interfaceDevice = static_cast<D3D12Device *>(device);
            RenderDescriptorRange descriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1);
            descriptorSet = interfaceDevice->createDescriptorSet(RenderDescriptorSetDesc(&descriptorRange, 1));

            D3D12DescriptorSet *interfaceDescriptorSet = static_cast<D3D12DescriptorSet *>(descriptorSet.get());
            const D3D12SwapChain *interfaceSwapChain = static_cast<const D3D12SwapChain *>(swapChain);
            const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = interfaceDevice->viewHeapAllocator->getCPUHandleAt(interfaceDescriptorSet->viewAllocation.offset);
            const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = interfaceDevice->viewHeapAllocator->getGPUHandleAt(interfaceDescriptorSet->viewAllocation.offset);
            ImGui_ImplDX12_Init(interfaceDevice->d3d, 2, interfaceSwapChain->nativeFormat, interfaceDevice->viewHeapAllocator->heap, cpuHandle, gpuHandle);
#       else
            assert(false && "Unsupported Graphics API.");
            return;
#       endif
            break;
        }
        case UserConfiguration::GraphicsAPI::Vulkan: {
#       if defined(__EMSCRIPTEN__)
            assert(false && "Unsupported Graphics API.");
            return;
#       else
            VulkanDevice *interfaceDevice = static_cast<VulkanDevice *>(device);
            const VulkanSwapChain *interfaceSwapChain = static_cast<const VulkanSwapChain *>(swapChain);
            ImGui_ImplVulkan_LoadFunctions([](const char *functionName, void *vulkanInstance) {
                return vkGetInstanceProcAddr(*(reinterpret_cast<VkInstance *>(vulkanInstance)), functionName);
            }, &interfaceDevice->renderInterface->instance);

            std::unordered_map<VkDescriptorType, uint32_t> typeCounts;
            typeCounts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 1;
            
            vulkanContext = std::make_unique<VulkanContext>();
            vulkanContext->device = interfaceDevice->vk;
            vulkanContext->renderPass = VulkanGraphicsPipeline::createRenderPass(interfaceDevice, &interfaceSwapChain->pickedSurfaceFormat.format, 1, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
            vulkanContext->descriptorPool = VulkanDescriptorSet::createDescriptorPool(interfaceDevice, typeCounts, false);

            ImGui_ImplVulkan_InitInfo initInfo = {};
            initInfo.Instance = interfaceDevice->renderInterface->instance;
            initInfo.PhysicalDevice = interfaceDevice->physicalDevice;
            initInfo.Device = vulkanContext->device;
            initInfo.QueueFamily = interfaceSwapChain->commandQueue->familyIndex;
            initInfo.Queue = interfaceSwapChain->commandQueue->queue->vk;
            initInfo.DescriptorPool = vulkanContext->descriptorPool;
            initInfo.RenderPass = vulkanContext->renderPass;
            initInfo.Subpass = 0;
            initInfo.MinImageCount = 2;
            initInfo.ImageCount = 2;
            initInfo.CheckVkResultFn = &checkVulkanResult;

            ImGui_ImplVulkan_Init(&initInfo);
#       endif
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            return;
        }
    }

    Inspector::~Inspector() {
#   if defined(__EMSCRIPTEN__)
        webgpuContext.reset();
        ImGui::GetIO().BackendRendererName = nullptr;
#   else
        switch (graphicsAPI) {
        case UserConfiguration::GraphicsAPI::D3D12: {
#       ifdef _WIN32
            ImGui_ImplDX12_Shutdown();
#       else
            assert(false && "Unsupported Graphics API.");
#       endif
            break;
        }
        case UserConfiguration::GraphicsAPI::Vulkan: {
            ImGui_ImplVulkan_Shutdown();
            vulkanContext.reset(nullptr);
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            break;
        }
#   endif

        if (sdlWindow != nullptr) {
            ImGui_ImplSDL2_Shutdown();
        }
        else {
#       ifdef _WIN32
            ImGui_ImplWin32_Shutdown();
#       endif
        }

        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    void Inspector::setIniPath(const std::filesystem::path &path) {
        ImGuiIO &io = ImGui::GetIO();
        IniFilenameUTF8 = path.u8string();
        io.IniFilename = IniFilenameUTF8.c_str();
    }
    
    void Inspector::newFrame(RenderWorker *worker) {
        assert(worker != nullptr);

        frameMutex.lock();
        
        if (sdlWindow != nullptr) {
            ImGui_ImplSDL2_NewFrame();
        }
        else {
#       ifdef _WIN32
            ImGui_ImplWin32_NewFrame();
#       endif
        }

#   if defined(__EMSCRIPTEN__)
        webgpuContext->updateFontTexture();
#   else
        switch (graphicsAPI) {
        case UserConfiguration::GraphicsAPI::D3D12: {
#       ifdef _WIN32
            ImGui_ImplDX12_NewFrame();
#       else
            assert(false && "Unsupported Graphics API.");
#       endif
            break;
        }
        case UserConfiguration::GraphicsAPI::Vulkan: {
            ImGui_ImplVulkan_NewFrame();
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            break;
        }
#   endif

        ImGui::NewFrame();
        Im3d::NewFrame();
    }

    void Inspector::endFrame() {
        ImGui::Render();
        Im3d::EndFrame();
        frameMutex.unlock();
    }

    void Inspector::draw(RenderCommandList *commandList) {
        const std::lock_guard<std::mutex> frameLock(frameMutex);
        ImDrawData *drawData = ImGui::GetDrawData();
        if (drawData != nullptr) {
#       if defined(__EMSCRIPTEN__)
            webgpuContext->draw(commandList, drawData, swapChain->getWidth(), swapChain->getHeight());
            return;
#       endif
            switch (graphicsAPI) {
            case UserConfiguration::GraphicsAPI::D3D12: {
#       ifdef _WIN32
                D3D12CommandList *interfaceCommandList = static_cast<D3D12CommandList *>(commandList);
                interfaceCommandList->checkDescriptorHeaps();
                ImGui_ImplDX12_RenderDrawData(drawData, interfaceCommandList->d3d);
#       else
                assert(false && "Unsupported Graphics API.");
#       endif
                break;
            }
            case UserConfiguration::GraphicsAPI::Vulkan: {
#       if !defined(__EMSCRIPTEN__)
                VulkanCommandList *interfaceCommandList = static_cast<VulkanCommandList *>(commandList);
                ImGui_ImplVulkan_RenderDrawData(drawData, interfaceCommandList->vk);
#       endif
                break;
            }
            default:
                assert(false && "Unknown Graphics API.");
                break;
            }
        }
    }

#ifdef _WIN32
    bool Inspector::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        return ImGui_ImplWin32_WndProcHandler(swapChain->getWindow(), msg, wParam, lParam);
    }
#endif

    bool Inspector::handleSdlEvent(SDL_Event *event) {
        assert((sdlWindow != nullptr) && "SDL Events shouldn't be handled if SDL was not used.");

        bool processed = ImGui_ImplSDL2_ProcessEvent(event);
        if (processed) {
            if ((event->type == SDL_KEYDOWN) || (event->type == SDL_KEYUP)) {
                return ImGui::GetIO().WantCaptureKeyboard;
            }
            else if ((event->type == SDL_MOUSEMOTION) || (event->type == SDL_MOUSEBUTTONDOWN) || (event->type == SDL_MOUSEBUTTONUP) || (event->type == SDL_MOUSEWHEEL)) {
                return ImGui::GetIO().WantCaptureMouse;
            }
        }
        
        return false;
    }
};
