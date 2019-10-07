// Copyright 2017 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dawn_native/vulkan/DeviceVk.h"

#include "common/Platform.h"
#include "dawn_native/BackendConnection.h"
#include "dawn_native/Commands.h"
#include "dawn_native/DynamicUploader.h"
#include "dawn_native/Error.h"
#include "dawn_native/ErrorData.h"
#include "dawn_native/VulkanBackend.h"
#include "dawn_native/vulkan/AdapterVk.h"
#include "dawn_native/vulkan/BackendVk.h"
#include "dawn_native/vulkan/BindGroupLayoutVk.h"
#include "dawn_native/vulkan/BindGroupVk.h"
#include "dawn_native/vulkan/BufferVk.h"
#include "dawn_native/vulkan/CommandBufferVk.h"
#include "dawn_native/vulkan/ComputePipelineVk.h"
#include "dawn_native/vulkan/FencedDeleter.h"
#include "dawn_native/vulkan/PipelineLayoutVk.h"
#include "dawn_native/vulkan/QueueVk.h"
#include "dawn_native/vulkan/RenderPassCache.h"
#include "dawn_native/vulkan/RenderPipelineVk.h"
#include "dawn_native/vulkan/SamplerVk.h"
#include "dawn_native/vulkan/ShaderModuleVk.h"
#include "dawn_native/vulkan/StagingBufferVk.h"
#include "dawn_native/vulkan/SwapChainVk.h"
#include "dawn_native/vulkan/TextureVk.h"
#include "dawn_native/vulkan/VulkanError.h"

namespace dawn_native { namespace vulkan {

    Device::Device(Adapter* adapter, const DeviceDescriptor* descriptor)
        : DeviceBase(adapter, descriptor) {
        InitTogglesFromDriver();
        if (descriptor != nullptr) {
            ApplyToggleOverrides(descriptor);
        }
    }

    MaybeError Device::Initialize() {
        // Copy the adapter's device info to the device so that we can change the "knobs"
        mDeviceInfo = ToBackend(GetAdapter())->GetDeviceInfo();

        VulkanFunctions* functions = GetMutableFunctions();
        *functions = ToBackend(GetAdapter())->GetBackend()->GetFunctions();

        VkPhysicalDevice physicalDevice = ToBackend(GetAdapter())->GetPhysicalDevice();

        VulkanDeviceKnobs usedDeviceKnobs = {};
        DAWN_TRY_ASSIGN(usedDeviceKnobs, CreateDevice(physicalDevice));
        *static_cast<VulkanDeviceKnobs*>(&mDeviceInfo) = usedDeviceKnobs;

        DAWN_TRY(functions->LoadDeviceProcs(mVkDevice, mDeviceInfo));

        GatherQueueFromDevice();
        mDeleter = std::make_unique<FencedDeleter>(this);
        mMapRequestTracker = std::make_unique<MapRequestTracker>(this);
        mMemoryAllocator = std::make_unique<MemoryAllocator>(this);
        mRenderPassCache = std::make_unique<RenderPassCache>(this);
        mResourceAllocator = std::make_unique<MemoryResourceAllocator>(this);

        mExternalMemoryService = std::make_unique<external_memory::Service>(this);
        mExternalSemaphoreService = std::make_unique<external_semaphore::Service>(this);

        return {};
    }

    Device::~Device() {
        // Immediately forget about all pending commands so we don't try to submit them in Tick
        FreeCommands(&mPendingCommands);

        if (fn.QueueWaitIdle(mQueue) != VK_SUCCESS) {
            ASSERT(false);
        }
        CheckPassedFences();

        // Make sure all fences are complete by explicitly waiting on them all
        while (!mFencesInFlight.empty()) {
            VkFence fence = mFencesInFlight.front().first;
            Serial fenceSerial = mFencesInFlight.front().second;
            ASSERT(fenceSerial > mCompletedSerial);

            VkResult result = VK_TIMEOUT;
            do {
                result = fn.WaitForFences(mVkDevice, 1, &fence, true, UINT64_MAX);
            } while (result == VK_TIMEOUT);
            fn.DestroyFence(mVkDevice, fence, nullptr);

            mFencesInFlight.pop();
            mCompletedSerial = fenceSerial;
        }

        // Some operations might have been started since the last submit and waiting
        // on a serial that doesn't have a corresponding fence enqueued. Force all
        // operations to look as if they were completed (because they were).
        mCompletedSerial = mLastSubmittedSerial + 1;
        Tick();

        ASSERT(mCommandsInFlight.Empty());
        for (auto& commands : mUnusedCommands) {
            FreeCommands(&commands);
        }
        mUnusedCommands.clear();

        ASSERT(mRecordingContext.waitSemaphores.empty());
        ASSERT(mRecordingContext.signalSemaphores.empty());

        for (VkFence fence : mUnusedFences) {
            fn.DestroyFence(mVkDevice, fence, nullptr);
        }
        mUnusedFences.clear();

        // Free services explicitly so that they can free Vulkan objects before vkDestroyDevice
        mDynamicUploader = nullptr;

        // Releasing the uploader enqueues buffers to be released.
        // Call Tick() again to clear them before releasing the deleter.
        mDeleter->Tick(mCompletedSerial);

        mDeleter = nullptr;
        mMapRequestTracker = nullptr;
        mMemoryAllocator = nullptr;

        // The VkRenderPasses in the cache can be destroyed immediately since all commands referring
        // to them are guaranteed to be finished executing.
        mRenderPassCache = nullptr;

        // VkQueues are destroyed when the VkDevice is destroyed
        if (mVkDevice != VK_NULL_HANDLE) {
            fn.DestroyDevice(mVkDevice, nullptr);
            mVkDevice = VK_NULL_HANDLE;
        }
    }

    ResultOrError<BindGroupBase*> Device::CreateBindGroupImpl(
        const BindGroupDescriptor* descriptor) {
        return new BindGroup(this, descriptor);
    }
    ResultOrError<BindGroupLayoutBase*> Device::CreateBindGroupLayoutImpl(
        const BindGroupLayoutDescriptor* descriptor) {
        return new BindGroupLayout(this, descriptor);
    }
    ResultOrError<BufferBase*> Device::CreateBufferImpl(const BufferDescriptor* descriptor) {
        std::unique_ptr<Buffer> buffer = std::make_unique<Buffer>(this, descriptor);
        DAWN_TRY(buffer->Initialize());
        return buffer.release();
    }
    CommandBufferBase* Device::CreateCommandBuffer(CommandEncoderBase* encoder,
                                                   const CommandBufferDescriptor* descriptor) {
        return new CommandBuffer(encoder, descriptor);
    }
    ResultOrError<ComputePipelineBase*> Device::CreateComputePipelineImpl(
        const ComputePipelineDescriptor* descriptor) {
        return new ComputePipeline(this, descriptor);
    }
    ResultOrError<PipelineLayoutBase*> Device::CreatePipelineLayoutImpl(
        const PipelineLayoutDescriptor* descriptor) {
        return new PipelineLayout(this, descriptor);
    }
    ResultOrError<QueueBase*> Device::CreateQueueImpl() {
        return new Queue(this);
    }
    ResultOrError<RenderPipelineBase*> Device::CreateRenderPipelineImpl(
        const RenderPipelineDescriptor* descriptor) {
        return new RenderPipeline(this, descriptor);
    }
    ResultOrError<SamplerBase*> Device::CreateSamplerImpl(const SamplerDescriptor* descriptor) {
        return new Sampler(this, descriptor);
    }
    ResultOrError<ShaderModuleBase*> Device::CreateShaderModuleImpl(
        const ShaderModuleDescriptor* descriptor) {
        return new ShaderModule(this, descriptor);
    }
    ResultOrError<SwapChainBase*> Device::CreateSwapChainImpl(
        const SwapChainDescriptor* descriptor) {
        return new SwapChain(this, descriptor);
    }
    ResultOrError<TextureBase*> Device::CreateTextureImpl(const TextureDescriptor* descriptor) {
        return new Texture(this, descriptor);
    }
    ResultOrError<TextureViewBase*> Device::CreateTextureViewImpl(
        TextureBase* texture,
        const TextureViewDescriptor* descriptor) {
        return new TextureView(texture, descriptor);
    }

    Serial Device::GetCompletedCommandSerial() const {
        return mCompletedSerial;
    }

    Serial Device::GetLastSubmittedCommandSerial() const {
        return mLastSubmittedSerial;
    }

    Serial Device::GetPendingCommandSerial() const {
        return mLastSubmittedSerial + 1;
    }

    MaybeError Device::TickImpl() {
        CheckPassedFences();
        RecycleCompletedCommands();

        mMapRequestTracker->Tick(mCompletedSerial);

        // Uploader should tick before the resource allocator
        // as it enqueues resources to be released.
        mDynamicUploader->Deallocate(mCompletedSerial);

        mMemoryAllocator->Tick(mCompletedSerial);

        mDeleter->Tick(mCompletedSerial);

        if (mPendingCommands.pool != VK_NULL_HANDLE) {
            SubmitPendingCommands();
        } else if (mCompletedSerial == mLastSubmittedSerial) {
            // If there's no GPU work in flight we still need to artificially increment the serial
            // so that CPU operations waiting on GPU completion can know they don't have to wait.
            mCompletedSerial++;
            mLastSubmittedSerial++;
        }

        return {};
    }

    VkInstance Device::GetVkInstance() const {
        return ToBackend(GetAdapter())->GetBackend()->GetVkInstance();
    }
    const VulkanDeviceInfo& Device::GetDeviceInfo() const {
        return mDeviceInfo;
    }

    VkDevice Device::GetVkDevice() const {
        return mVkDevice;
    }

    uint32_t Device::GetGraphicsQueueFamily() const {
        return mQueueFamily;
    }

    VkQueue Device::GetQueue() const {
        return mQueue;
    }

    MapRequestTracker* Device::GetMapRequestTracker() const {
        return mMapRequestTracker.get();
    }

    MemoryAllocator* Device::GetMemoryAllocator() const {
        return mMemoryAllocator.get();
    }

    FencedDeleter* Device::GetFencedDeleter() const {
        return mDeleter.get();
    }

    RenderPassCache* Device::GetRenderPassCache() const {
        return mRenderPassCache.get();
    }

    VkCommandBuffer Device::GetPendingCommandBuffer() {
        if (mPendingCommands.pool == VK_NULL_HANDLE) {
            mPendingCommands = GetUnusedCommands();

            VkCommandBufferBeginInfo beginInfo;
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.pNext = nullptr;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            beginInfo.pInheritanceInfo = nullptr;

            if (fn.BeginCommandBuffer(mPendingCommands.commandBuffer, &beginInfo) != VK_SUCCESS) {
                ASSERT(false);
            }
        }

        return mPendingCommands.commandBuffer;
    }

    CommandRecordingContext* Device::GetPendingRecordingContext() {
        if (mRecordingContext.commandBuffer == VK_NULL_HANDLE) {
            mRecordingContext.commandBuffer = GetPendingCommandBuffer();
        }

        return &mRecordingContext;
    }

    void Device::SubmitPendingCommands() {
        if (mPendingCommands.pool == VK_NULL_HANDLE) {
            return;
        }

        if (fn.EndCommandBuffer(mPendingCommands.commandBuffer) != VK_SUCCESS) {
            ASSERT(false);
        }

        std::vector<VkPipelineStageFlags> dstStageMasks(mRecordingContext.waitSemaphores.size(),
                                                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        VkSubmitInfo submitInfo;
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount =
            static_cast<uint32_t>(mRecordingContext.waitSemaphores.size());
        submitInfo.pWaitSemaphores = mRecordingContext.waitSemaphores.data();
        submitInfo.pWaitDstStageMask = dstStageMasks.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &mPendingCommands.commandBuffer;
        submitInfo.signalSemaphoreCount =
            static_cast<uint32_t>(mRecordingContext.signalSemaphores.size());
        submitInfo.pSignalSemaphores = mRecordingContext.signalSemaphores.data();

        VkFence fence = GetUnusedFence();
        if (fn.QueueSubmit(mQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
            ASSERT(false);
        }

        mLastSubmittedSerial++;
        mCommandsInFlight.Enqueue(mPendingCommands, mLastSubmittedSerial);
        mPendingCommands = CommandPoolAndBuffer();
        mFencesInFlight.emplace(fence, mLastSubmittedSerial);

        for (VkSemaphore semaphore : mRecordingContext.waitSemaphores) {
            mDeleter->DeleteWhenUnused(semaphore);
        }

        for (VkSemaphore semaphore : mRecordingContext.signalSemaphores) {
            mDeleter->DeleteWhenUnused(semaphore);
        }

        mRecordingContext = CommandRecordingContext();
    }

    ResultOrError<VulkanDeviceKnobs> Device::CreateDevice(VkPhysicalDevice physicalDevice) {
        VulkanDeviceKnobs usedKnobs = {};

        float zero = 0.0f;
        std::vector<const char*> layersToRequest;
        std::vector<const char*> extensionsToRequest;
        std::vector<VkDeviceQueueCreateInfo> queuesToRequest;

        if (mDeviceInfo.debugMarker) {
            extensionsToRequest.push_back(kExtensionNameExtDebugMarker);
            usedKnobs.debugMarker = true;
        }
        if (mDeviceInfo.externalMemory) {
            extensionsToRequest.push_back(kExtensionNameKhrExternalMemory);
            usedKnobs.externalMemory = true;
        }
        if (mDeviceInfo.externalMemoryFD) {
            extensionsToRequest.push_back(kExtensionNameKhrExternalMemoryFD);
            usedKnobs.externalMemoryFD = true;
        }
        if (mDeviceInfo.externalMemoryZirconHandle) {
            extensionsToRequest.push_back(kExtensionNameFuchsiaExternalMemory);
            usedKnobs.externalMemoryZirconHandle = true;
        }
        if (mDeviceInfo.externalSemaphore) {
            extensionsToRequest.push_back(kExtensionNameKhrExternalSemaphore);
            usedKnobs.externalSemaphore = true;
        }
        if (mDeviceInfo.externalSemaphoreFD) {
            extensionsToRequest.push_back(kExtensionNameKhrExternalSemaphoreFD);
            usedKnobs.externalSemaphoreFD = true;
        }
        if (mDeviceInfo.externalSemaphoreZirconHandle) {
            extensionsToRequest.push_back(kExtensionNameFuchsiaExternalSemaphore);
            usedKnobs.externalSemaphoreZirconHandle = true;
        }
        if (mDeviceInfo.swapchain) {
            extensionsToRequest.push_back(kExtensionNameKhrSwapchain);
            usedKnobs.swapchain = true;
        }
        if (mDeviceInfo.maintenance1) {
            extensionsToRequest.push_back(kExtensionNameKhrMaintenance1);
            usedKnobs.maintenance1 = true;
        }

        // Always require independentBlend because it is a core Dawn feature
        usedKnobs.features.independentBlend = VK_TRUE;
        // Always require imageCubeArray because it is a core Dawn feature
        usedKnobs.features.imageCubeArray = VK_TRUE;
        // Always require fragmentStoresAndAtomics because it is required by end2end tests.
        usedKnobs.features.fragmentStoresAndAtomics = VK_TRUE;

        if (IsExtensionEnabled(Extension::TextureCompressionBC)) {
            ASSERT(ToBackend(GetAdapter())->GetDeviceInfo().features.textureCompressionBC ==
                   VK_TRUE);
            usedKnobs.features.textureCompressionBC = VK_TRUE;
        }

        // Find a universal queue family
        {
            constexpr uint32_t kUniversalFlags =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            int universalQueueFamily = -1;
            for (unsigned int i = 0; i < mDeviceInfo.queueFamilies.size(); ++i) {
                if ((mDeviceInfo.queueFamilies[i].queueFlags & kUniversalFlags) ==
                    kUniversalFlags) {
                    universalQueueFamily = i;
                    break;
                }
            }

            if (universalQueueFamily == -1) {
                return DAWN_DEVICE_LOST_ERROR("No universal queue family");
            }
            mQueueFamily = static_cast<uint32_t>(universalQueueFamily);
        }

        // Choose to create a single universal queue
        {
            VkDeviceQueueCreateInfo queueCreateInfo;
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.pNext = nullptr;
            queueCreateInfo.flags = 0;
            queueCreateInfo.queueFamilyIndex = static_cast<uint32_t>(mQueueFamily);
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &zero;

            queuesToRequest.push_back(queueCreateInfo);
        }

        VkDeviceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queuesToRequest.size());
        createInfo.pQueueCreateInfos = queuesToRequest.data();
        createInfo.enabledLayerCount = static_cast<uint32_t>(layersToRequest.size());
        createInfo.ppEnabledLayerNames = layersToRequest.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensionsToRequest.size());
        createInfo.ppEnabledExtensionNames = extensionsToRequest.data();
        createInfo.pEnabledFeatures = &usedKnobs.features;

        DAWN_TRY(CheckVkSuccess(fn.CreateDevice(physicalDevice, &createInfo, nullptr, &mVkDevice),
                                "vkCreateDevice"));

        return usedKnobs;
    }

    void Device::GatherQueueFromDevice() {
        fn.GetDeviceQueue(mVkDevice, mQueueFamily, 0, &mQueue);
    }

    void Device::InitTogglesFromDriver() {
        // TODO(jiawei.shao@intel.com): tighten this workaround when this issue is fixed in both
        // Vulkan SPEC and drivers.
        SetToggle(Toggle::UseTemporaryBufferInCompressedTextureToTextureCopy, true);
    }

    VulkanFunctions* Device::GetMutableFunctions() {
        return const_cast<VulkanFunctions*>(&fn);
    }

    VkFence Device::GetUnusedFence() {
        if (!mUnusedFences.empty()) {
            VkFence fence = mUnusedFences.back();
            mUnusedFences.pop_back();
            return fence;
        }

        VkFenceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;

        VkFence fence = VK_NULL_HANDLE;
        if (fn.CreateFence(mVkDevice, &createInfo, nullptr, &fence) != VK_SUCCESS) {
            ASSERT(false);
        }

        return fence;
    }

    void Device::CheckPassedFences() {
        while (!mFencesInFlight.empty()) {
            VkFence fence = mFencesInFlight.front().first;
            Serial fenceSerial = mFencesInFlight.front().second;

            VkResult result = fn.GetFenceStatus(mVkDevice, fence);
            ASSERT(result == VK_SUCCESS || result == VK_NOT_READY);

            // Fence are added in order, so we can stop searching as soon
            // as we see one that's not ready.
            if (result == VK_NOT_READY) {
                return;
            }

            if (fn.ResetFences(mVkDevice, 1, &fence) != VK_SUCCESS) {
                ASSERT(false);
            }
            mUnusedFences.push_back(fence);

            mFencesInFlight.pop();

            ASSERT(fenceSerial > mCompletedSerial);
            mCompletedSerial = fenceSerial;
        }
    }

    Device::CommandPoolAndBuffer Device::GetUnusedCommands() {
        if (!mUnusedCommands.empty()) {
            CommandPoolAndBuffer commands = mUnusedCommands.back();
            mUnusedCommands.pop_back();
            return commands;
        }

        CommandPoolAndBuffer commands;

        VkCommandPoolCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        createInfo.queueFamilyIndex = mQueueFamily;

        if (fn.CreateCommandPool(mVkDevice, &createInfo, nullptr, &commands.pool) != VK_SUCCESS) {
            ASSERT(false);
        }

        VkCommandBufferAllocateInfo allocateInfo;
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.pNext = nullptr;
        allocateInfo.commandPool = commands.pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;

        if (fn.AllocateCommandBuffers(mVkDevice, &allocateInfo, &commands.commandBuffer) !=
            VK_SUCCESS) {
            ASSERT(false);
        }

        return commands;
    }

    void Device::RecycleCompletedCommands() {
        for (auto& commands : mCommandsInFlight.IterateUpTo(mCompletedSerial)) {
            if (fn.ResetCommandPool(mVkDevice, commands.pool, 0) != VK_SUCCESS) {
                ASSERT(false);
            }
            mUnusedCommands.push_back(commands);
        }
        mCommandsInFlight.ClearUpTo(mCompletedSerial);
    }

    void Device::FreeCommands(CommandPoolAndBuffer* commands) {
        if (commands->pool != VK_NULL_HANDLE) {
            fn.DestroyCommandPool(mVkDevice, commands->pool, nullptr);
            commands->pool = VK_NULL_HANDLE;
        }

        // Command buffers are implicitly destroyed when the command pool is.
        commands->commandBuffer = VK_NULL_HANDLE;
    }

    ResultOrError<std::unique_ptr<StagingBufferBase>> Device::CreateStagingBuffer(size_t size) {
        std::unique_ptr<StagingBufferBase> stagingBuffer =
            std::make_unique<StagingBuffer>(size, this);
        DAWN_TRY(stagingBuffer->Initialize());
        return std::move(stagingBuffer);
    }

    MaybeError Device::CopyFromStagingToBuffer(StagingBufferBase* source,
                                               uint64_t sourceOffset,
                                               BufferBase* destination,
                                               uint64_t destinationOffset,
                                               uint64_t size) {
        // Insert memory barrier to ensure host write operations are made visible before
        // copying from the staging buffer. However, this barrier can be removed (see note below).
        //
        // Note: Depending on the spec understanding, an explicit barrier may not be required when
        // used with HOST_COHERENT as vkQueueSubmit does an implicit barrier between host and
        // device. See "Availability, Visibility, and Domain Operations" in Vulkan spec for details.

        // Insert pipeline barrier to ensure correct ordering with previous memory operations on the
        // buffer.
        ToBackend(destination)
            ->TransitionUsageNow(GetPendingRecordingContext(), dawn::BufferUsage::CopyDst);

        VkBufferCopy copy;
        copy.srcOffset = sourceOffset;
        copy.dstOffset = destinationOffset;
        copy.size = size;

        this->fn.CmdCopyBuffer(GetPendingCommandBuffer(), ToBackend(source)->GetBufferHandle(),
                               ToBackend(destination)->GetHandle(), 1, &copy);

        return {};
    }

    MaybeError Device::ImportExternalImage(const ExternalImageDescriptor* descriptor,
                                           ExternalMemoryHandle memoryHandle,
                                           const std::vector<ExternalSemaphoreHandle>& waitHandles,
                                           VkSemaphore* outSignalSemaphore,
                                           VkDeviceMemory* outAllocation,
                                           std::vector<VkSemaphore>* outWaitSemaphores) {
        const TextureDescriptor* textureDescriptor =
            reinterpret_cast<const TextureDescriptor*>(descriptor->cTextureDescriptor);

        // Check services support this combination of handle type / image info
        if (!mExternalSemaphoreService->Supported()) {
            return DAWN_VALIDATION_ERROR("External semaphore usage not supported");
        }
        if (!mExternalMemoryService->Supported(
                VulkanImageFormat(textureDescriptor->format), VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                VulkanImageUsage(textureDescriptor->usage,
                                 GetValidInternalFormat(textureDescriptor->format)),
                VK_IMAGE_CREATE_ALIAS_BIT_KHR)) {
            return DAWN_VALIDATION_ERROR("External memory usage not supported");
        }

        // Create an external semaphore to signal when the texture is done being used
        DAWN_TRY_ASSIGN(*outSignalSemaphore,
                        mExternalSemaphoreService->CreateExportableSemaphore());

        // Import the external image's memory
        DAWN_TRY_ASSIGN(*outAllocation,
                        mExternalMemoryService->ImportMemory(
                            memoryHandle, descriptor->allocationSize, descriptor->memoryTypeIndex));

        // Import semaphores we have to wait on before using the texture
        for (const ExternalSemaphoreHandle& handle : waitHandles) {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            DAWN_TRY_ASSIGN(semaphore, mExternalSemaphoreService->ImportSemaphore(handle));
            outWaitSemaphores->push_back(semaphore);
        }

        return {};
    }

    MaybeError Device::SignalAndExportExternalTexture(Texture* texture,
                                                      ExternalSemaphoreHandle* outHandle) {
        DAWN_TRY(ValidateObject(texture));

        VkSemaphore outSignalSemaphore;
        DAWN_TRY(texture->SignalAndDestroy(&outSignalSemaphore));

        // This has to happen right after SignalAndDestroy, since the semaphore will be
        // deleted when the fenced deleter runs after the queue submission
        DAWN_TRY_ASSIGN(*outHandle, mExternalSemaphoreService->ExportSemaphore(outSignalSemaphore));

        return {};
    }

    TextureBase* Device::CreateTextureWrappingVulkanImage(
        const ExternalImageDescriptor* descriptor,
        ExternalMemoryHandle memoryHandle,
        const std::vector<ExternalSemaphoreHandle>& waitHandles) {
        const TextureDescriptor* textureDescriptor =
            reinterpret_cast<const TextureDescriptor*>(descriptor->cTextureDescriptor);

        // Initial validation
        if (ConsumedError(ValidateTextureDescriptor(this, textureDescriptor))) {
            return nullptr;
        }
        if (ConsumedError(ValidateVulkanImageCanBeWrapped(this, textureDescriptor))) {
            return nullptr;
        }

        VkSemaphore signalSemaphore = VK_NULL_HANDLE;
        VkDeviceMemory allocation = VK_NULL_HANDLE;
        std::vector<VkSemaphore> waitSemaphores;
        waitSemaphores.reserve(waitHandles.size());

        // If failed, cleanup
        if (ConsumedError(ImportExternalImage(descriptor, memoryHandle, waitHandles,
                                              &signalSemaphore, &allocation, &waitSemaphores))) {
            // Clear the signal semaphore
            fn.DestroySemaphore(GetVkDevice(), signalSemaphore, nullptr);

            // Clear image memory
            fn.FreeMemory(GetVkDevice(), allocation, nullptr);

            // Clear any wait semaphores we were able to import
            for (VkSemaphore semaphore : waitSemaphores) {
                fn.DestroySemaphore(GetVkDevice(), semaphore, nullptr);
            }
            return nullptr;
        }

        return new Texture(this, descriptor, textureDescriptor, signalSemaphore, allocation,
                           waitSemaphores);
    }

    ResultOrError<ResourceMemoryAllocation> Device::AllocateMemory(
        VkMemoryRequirements requirements,
        bool mappable) {
        // TODO(crbug.com/dawn/27): Support sub-allocation.
        ResourceMemoryAllocation allocation;
        DAWN_TRY_ASSIGN(allocation, mResourceAllocator->Allocate(requirements, mappable));
        return allocation;
    }

    void Device::DeallocateMemory(ResourceMemoryAllocation& allocation) {
        if (allocation.GetInfo().mMethod == AllocationMethod::kInvalid) {
            return;
        }
        mResourceAllocator->Deallocate(allocation);

        // Invalidate the underlying resource heap in case the client accidentally
        // calls DeallocateMemory again using the same allocation.
        allocation.Invalidate();
    }
}}  // namespace dawn_native::vulkan
