/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <unordered_set>

#include "include/sl.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.plugin-manager/pluginManager.h"
#include "source/core/sl.interposer/vulkan/layer.h"
#include "source/core/sl.interposer/hook.h"
#include "include/sl_hooks.h"
#include "include/sl_helpers_vk.h"
#include "include/sl_struct.h"

HMODULE s_module = {};

using namespace sl::interposer;

VkTable s_vk{};
VkLayerInstanceDispatchTable s_idt{};
VkLayerDispatchTable s_ddt{};

HMODULE loadVulkanLibrary()
{
    if (!s_module)
    {
#ifdef SL_WINDOWS
        s_module = ::LoadLibraryA("vulkan-1.dll");
#else
        s_module = ::LoadLibraryA("vulkan-1.so");
#endif
    }
    return s_module;
}

//! Only used when manually hooking Vulkan API
//! 
//! Host is in charge and providing information we need
//! 
sl::Result processVulkanInterface(const sl::VulkanInfo* extension)
{
    if (!loadVulkanLibrary())
    {
        SL_LOG_ERROR( "Failed to load Vulkan library");
        return sl::Result::eErrorVulkanAPI;
    }

    s_vk.instance = extension->instance;
    s_vk.device = extension->device;
    s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
    s_vk.getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
    s_vk.graphicsQueueFamily = extension->graphicsQueueFamily;
    s_vk.graphicsQueueIndex = extension->graphicsQueueIndex;
    s_vk.computeQueueFamily = extension->computeQueueFamily;
    s_vk.computeQueueIndex = extension->computeQueueIndex;
    if (extension->structVersion >= sl::kStructVersion3)
    {
        s_vk.graphicsQueueCreateFlags = extension->graphicsQueueCreateFlags;
        s_vk.computeQueueCreateFlags = extension->computeQueueCreateFlags;
        s_vk.opticalFlowQueueCreateFlags = extension->opticalFlowQueueCreateFlags;
    }
    if (extension->structVersion >= sl::kStructVersion2)
    {
        s_vk.opticalFlowQueueFamily = extension->opticalFlowQueueFamily;
        s_vk.opticalFlowQueueIndex = extension->opticalFlowQueueIndex;
        s_vk.nativeOpticalFlowHWSupport = extension->useNativeOpticalFlowMode;
    }

    s_vk.mapVulkanInstanceAPI(s_vk.instance);
    s_idt = s_vk.dispatchInstanceMap[s_vk.instance];

    s_vk.mapVulkanDeviceAPI(s_vk.device);
    s_ddt = s_vk.dispatchDeviceMap[s_vk.device];

    // Allow all plugins to access this information
    sl::param::getInterface()->set(sl::param::global::kVulkanTable, &s_vk);

    return sl::Result::eOk;
}

extern "C"
{
    // -- Vulkan 1.0 ---

    PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
    PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);

    VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
        s_vk.getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
        auto createInstance = (PFN_vkCreateInstance)GetProcAddress(s_module, "vkCreateInstance");

        if (!createInstance)
        {
            SL_LOG_ERROR( "Failed to map vkCreateInstance");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        if (pCreateInfo->pApplicationInfo)
        {
            appInfo = *pCreateInfo->pApplicationInfo;
        }

        if (appInfo.apiVersion < VK_API_VERSION_1_3)
        {
            appInfo.apiVersion = VK_API_VERSION_1_3;
        }

        VkInstanceCreateInfo createInfo = *pCreateInfo;
        createInfo.pApplicationInfo = &appInfo;

        // Build up a list of extensions to enable
        std::unordered_set<std::string> requiredSLInstanceExtensionNames =
        {
#ifndef SL_PRODUCTION
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
        };

        auto pluginManager = sl::plugin_manager::getInterface();
        std::vector<json> configs;
        pluginManager->getLoadedFeatureConfigs(configs);
        for (auto& cfg : configs)
        {
            if (cfg.contains("/external/vk/instance/extensions"_json_pointer))
            {
                std::vector<std::string> pluginExtensions;
                cfg["external"]["vk"]["instance"]["extensions"].get_to(pluginExtensions);
                for (auto ext : pluginExtensions)
                {
                    auto res = requiredSLInstanceExtensionNames.insert(ext);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding instance extension '%s'", ext.c_str());
                    }
                }
            }
        }

        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++)
        {
            requiredSLInstanceExtensionNames.insert(createInfo.ppEnabledExtensionNames[i]);
        }

        uint32_t instanceExtensionCount{};
        VK_CHECK_RI(vkEnumerateInstanceExtensionProperties(NULL, &instanceExtensionCount, NULL));
        if (instanceExtensionCount == 0)
        {
            SL_LOG_ERROR("No supported instance extensions enumerated!");
        }
        std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
        VK_CHECK_RI(vkEnumerateInstanceExtensionProperties(NULL, &instanceExtensionCount, availableInstanceExtensions.data()));

        std::unordered_set<std::string> unsupportedInstanceExtensionNames(requiredSLInstanceExtensionNames);
        for (const auto& ext : availableInstanceExtensions)
        {
            unsupportedInstanceExtensionNames.erase(ext.extensionName);
        }
        if (!unsupportedInstanceExtensionNames.empty())
        {
            for (const auto& ext : unsupportedInstanceExtensionNames)
            {
                SL_LOG_ERROR("Required instance extension %s unsupported", ext.c_str());
                requiredSLInstanceExtensionNames.erase(ext);
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }

        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++)
        {
            requiredSLInstanceExtensionNames.insert(createInfo.ppEnabledExtensionNames[i]);
        }

        std::vector<const char*> extensions{};
        extensions.reserve(requiredSLInstanceExtensionNames.size());
        for (auto& e : requiredSLInstanceExtensionNames)
        {
            extensions.push_back(e.c_str());
        }

        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

#ifndef SL_PRODUCTION
        // Extra layers
        std::vector<const char*> layers;
        for (uint32_t i = 0; i < createInfo.enabledLayerCount; i++)
        {
            layers.push_back(createInfo.ppEnabledLayerNames[i]);
        }

        if (sl::interposer::getInterface()->getConfig().vkValidation)
        {
            if (std::find(layers.begin(), layers.end(), "VK_LAYER_KHRONOS_validation") == layers.end())
            {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                sl::param::getInterface()->set(sl::param::interposer::kVKValidationActive, true);
            }
        }

        createInfo.enabledLayerCount = (uint32_t)layers.size();
        createInfo.ppEnabledLayerNames = layers.data();
#endif
        auto res = createInstance(&createInfo, pAllocator, pInstance);
        if (res != VK_SUCCESS)
        {
            SL_LOG_ERROR( "vkCreateInstance failed");
            return res;
        }

        s_vk.instance = *pInstance;

        s_vk.mapVulkanInstanceAPI(s_vk.instance);

        s_idt = s_vk.dispatchInstanceMap[s_vk.instance];

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkEnumerateInstanceExtensionProperties)GetProcAddress(lib, "vkEnumerateInstanceExtensionProperties");
        return trampoline(pLayerName, pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkEnumerateInstanceLayerProperties)GetProcAddress(lib, "vkEnumerateInstanceLayerProperties");
        return trampoline(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto createInfo = *pCreateInfo;

        // Enable features SL requires
        std::unordered_set<std::string> requiredSLPhysicalDevice12FeatureNames =
        {
            "timelineSemaphore",
            "descriptorIndexing",
            "bufferDeviceAddress"
        };
        std::unordered_set<std::string> requiredSLPhysicalDevice13FeatureNames{};
        std::unordered_set<std::string> requiredSLOpticalFlowNVFeatureNames{};
        // Enable extra device extensions SL requires
        std::unordered_set<std::string> requiredSLDeviceExtensionNames
        {
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };

        // Figure out what extra features we need 
        uint32_t extraGraphicsQueues = 0;
        uint32_t extraComputeQueues = 0;
        uint32_t extraOpticalFlowQueues = 0;
        auto pluginManager = sl::plugin_manager::getInterface();

        std::vector<json> configs;
        pluginManager->getLoadedFeatureConfigs(configs);
        for (auto& cfg : configs)
        {
            // similarly add support for Vk 1.1 features, if any plugin requests such a feature.
            // 1.2 Features
            if (cfg.contains("/external/vk/device/1.2_features"_json_pointer))
            {
                std::vector<std::string> requiredSLPluginPhysicalDevice12FeatureNames{};
                cfg["external"]["vk"]["device"]["1.2_features"].get_to(requiredSLPluginPhysicalDevice12FeatureNames);
                for (auto feature : requiredSLPluginPhysicalDevice12FeatureNames)
                {
                    auto res = requiredSLPhysicalDevice12FeatureNames.insert(feature);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding device feature '%s' requested by a plugin(s)", feature.c_str());
                    }
                }
            }

            // 1.3 Features
            if (cfg.contains("/external/vk/device/1.3_features"_json_pointer))
            {
                std::vector<std::string> requiredSLPluginPhysicalDevice13FeatureNames{};
                cfg["external"]["vk"]["device"]["1.3_features"].get_to(requiredSLPluginPhysicalDevice13FeatureNames);
                for (auto feature : requiredSLPluginPhysicalDevice13FeatureNames)
                {
                    auto res = requiredSLPhysicalDevice13FeatureNames.insert(feature);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding device feature '%s' requested by a plugin(s)", feature.c_str());
                    }
                }
            }

            // Device extensions
            if (cfg.contains("/external/vk/device/extensions"_json_pointer))
            {
                std::vector<std::string> requiredSLPluginDeviceExtensionNames;
                cfg["external"]["vk"]["device"]["extensions"].get_to(requiredSLPluginDeviceExtensionNames);
                for (auto ext : requiredSLPluginDeviceExtensionNames)
                {
                    auto res = requiredSLDeviceExtensionNames.insert(ext);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding device extension '%s' requested by a plugin(s)", ext.c_str());
                    }
                }
            }

            // Additional queues?
            if (cfg.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            {
                extraGraphicsQueues += cfg["external"]["vk"]["device"]["queues"]["graphics"]["count"];
                SL_LOG_INFO("Adding extra %u graphics queue(s) requested by a plugin(s)", extraGraphicsQueues);
            }
            if (cfg.contains("/external/vk/device/queues/compute/count"_json_pointer))
            {
                extraComputeQueues += cfg["external"]["vk"]["device"]["queues"]["compute"]["count"];
                SL_LOG_INFO("Adding extra %u compute queue(s) requested by a plugin(s)", extraComputeQueues);
            }

            if (cfg.contains("/external/vk/opticalflow/supported"_json_pointer))
            {
                s_vk.nativeOpticalFlowHWSupport = cfg["external"]["vk"]["opticalflow"]["supported"];
                SL_LOG_INFO("Vulkan optical flow is supported natively as indicated by a plugin(s)");
            }
            if (s_vk.nativeOpticalFlowHWSupport)
            {
                // opticalflowNV Features
                if (cfg.contains("/external/vk/device/opticalflowNV_features"_json_pointer))
                {
                    std::vector<std::string> requiredSLPluginOpticalFlowNVFeatureNames{};
                    cfg["external"]["vk"]["device"]["opticalflowNV_features"].get_to(requiredSLPluginOpticalFlowNVFeatureNames);
                    for (auto feature : requiredSLPluginOpticalFlowNVFeatureNames)
                    {
                        auto res = requiredSLOpticalFlowNVFeatureNames.insert(feature);
                        if (res.second)
                        {
                            SL_LOG_INFO("Adding device feature '%s' requested by a plugin(s)", feature.c_str());
                        }
                    }
                }
                if (cfg.contains("/external/vk/device/queues/opticalflow/family"_json_pointer))
                {
                    s_vk.opticalFlowQueueFamily = cfg["external"]["vk"]["device"]["queues"]["opticalflow"]["family"];
                }

                if (cfg.contains("/external/vk/device/queues/opticalflow/count"_json_pointer))
                {
                    extraOpticalFlowQueues = cfg["external"]["vk"]["device"]["queues"]["opticalflow"]["count"];
                    SL_LOG_INFO("Adding extra %u optical flow queue(s) from queue family %u requested by a plugin(s)", extraOpticalFlowQueues, s_vk.opticalFlowQueueFamily);
                }
            }
        }

        std::vector<const char*> requiredPhysicalDevice12FeatureNames{};
        for (auto& feature : requiredSLPhysicalDevice12FeatureNames)
        {
            requiredPhysicalDevice12FeatureNames.emplace_back(feature.c_str());
        }
        // All VK 1.2 features required by SL and its plugins
        VkPhysicalDeviceVulkan12Features requiredSLPhysicalDevice12Features = sl::getVkPhysicalDeviceVulkan12Features(static_cast<uint32_t>(requiredPhysicalDevice12FeatureNames.size()), requiredPhysicalDevice12FeatureNames.data());

        std::vector<const char*> requiredPhysicalDevice13FeatureNames{};
        for (auto& feature : requiredSLPhysicalDevice13FeatureNames)
        {
            requiredPhysicalDevice13FeatureNames.emplace_back(feature.c_str());
        }
        // All VK 1.3 features required by SL and its plugins
        VkPhysicalDeviceVulkan13Features requiredSLPhysicalDevice13Features = sl::getVkPhysicalDeviceVulkan13Features(static_cast<uint32_t>(requiredPhysicalDevice13FeatureNames.size()), requiredPhysicalDevice13FeatureNames.data());

        std::vector<const char*> requiredOpticalflowNVFeatureNames{};
        for (auto& feature : requiredSLOpticalFlowNVFeatureNames)
        {
            requiredOpticalflowNVFeatureNames.emplace_back(feature.c_str());
        }
        // VK optical flow feature, if required by SL or its plugins
        VkPhysicalDeviceOpticalFlowFeaturesNV requiredSLOpticalFlowNVFeatures = sl::getVkPhysicalDeviceOpticalFlowNVFeatures(static_cast<uint32_t>(requiredOpticalflowNVFeatureNames.size()), requiredOpticalflowNVFeatureNames.data());

        VkPhysicalDeviceOpticalFlowFeaturesNV supportedOpticalFlowFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
        VkPhysicalDeviceVulkan13Features supportedPhysicalDevice13Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &supportedOpticalFlowFeaturesNV };
        VkPhysicalDeviceVulkan12Features supportedPhysicalDevice12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &supportedPhysicalDevice13Features };
        VkPhysicalDeviceFeatures2 supportedFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supportedPhysicalDevice12Features };
        // Query device support for VK 1.2, 1.3 and optical flow features and only enable supported ones below.
        vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);

        // Check if host is already specifying 1.2 features
        VkPhysicalDeviceVulkan12Features*                       pClientPhysicalDevice12Features{};
        VkPhysicalDevice8BitStorageFeatures*                    pClient8BitStorageFeatures{};
        VkPhysicalDeviceShaderAtomicInt64Features*              pClientShaderAtomicInt64Features{};
        VkPhysicalDeviceShaderFloat16Int8Features               shaderFloat16Int8Features{}, *pClientShaderFloat16Int8Features{};
        VkPhysicalDeviceDescriptorIndexingFeatures*             pClientDescriptorIndexingFeatures{};
        VkPhysicalDeviceScalarBlockLayoutFeatures*              pClientScalarBlockLayoutFeatures{};
        VkPhysicalDeviceImagelessFramebufferFeatures*           pClientImagelessFramebufferFeatures{};
        VkPhysicalDeviceUniformBufferStandardLayoutFeatures*    pClientUniformBufferStandardLayoutFeatures{};
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*    pClientShaderSubgroupExtendedTypesFeatures{};
        VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*    pClientSeparateDepthStencilLayoutsFeatures{};
        VkPhysicalDeviceHostQueryResetFeatures*                 pClientHostQueryResetFeatures{};
        VkPhysicalDeviceTimelineSemaphoreFeatures               timelineSemaphoreFeatures{}, *pClientTimelineSemaphoreFeatures{};
        VkPhysicalDeviceBufferDeviceAddressFeatures             bufferDeviceAddressFeatures{}, *pClientBufferDeviceAddressFeatures{};
        VkPhysicalDeviceVulkanMemoryModelFeatures*              pClientVulkanMemoryModelFeatures{};

        // Check if host is already specifying 1.3 features
        VkPhysicalDeviceVulkan13Features*                       pClientPhysicalDevice13Features{};
        VkPhysicalDeviceDynamicRenderingFeatures*               pClientDynamicRenderingFeatures{};
        VkPhysicalDeviceImageRobustnessFeatures*                pClientImageRobustnessFeatures{};
        VkPhysicalDeviceInlineUniformBlockFeatures*             pClientInlineUniformBlockFeatures{};
        VkPhysicalDeviceMaintenance4Features*                   pClientMaintenance4Features{};
        VkPhysicalDevicePipelineCreationCacheControlFeatures*   pClientPipelineCreationCacheControlFeatures{};
        VkPhysicalDevicePrivateDataFeatures*                    pClientPrivateDataFeatures{};
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures* pClientShaderDemoteToHelperInvocationFeatures{};
        VkPhysicalDeviceShaderIntegerDotProductFeatures*        pClientShaderIntegerDotProductFeatures{};
        VkPhysicalDeviceShaderTerminateInvocationFeatures*      pClientShaderTerminateInvocationFeatures{};
        VkPhysicalDeviceSubgroupSizeControlFeatures*            pClientSubgroupSizeControlFeatures{};
        VkPhysicalDeviceSynchronization2Features                synchronization2Features, *pClientSynchronization2Features{};
        VkPhysicalDeviceTextureCompressionASTCHDRFeatures*      pClientTextureCompressionASTCHDRFeatures{};
        VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures*  pClientZeroInitializeWorkgroupMemoryFeatures{};

        VkPhysicalDeviceOpticalFlowFeaturesNV*                  pClientOpticalFlowFeaturesNV{};

        VkBaseOutStructure*                                     pClientFeaturesChain = (VkBaseOutStructure*)(createInfo.pNext);

        while (pClientFeaturesChain != NULL)
        {
            switch (pClientFeaturesChain->sType)
            {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
                // VUID-VkDeviceCreateInfo-pNext-00373
                assert(createInfo.pEnabledFeatures == NULL);
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                pClientPhysicalDevice12Features = (VkPhysicalDeviceVulkan12Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
                pClient8BitStorageFeatures = (VkPhysicalDevice8BitStorageFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
                pClientShaderAtomicInt64Features = (VkPhysicalDeviceShaderAtomicInt64Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
                pClientShaderFloat16Int8Features = (VkPhysicalDeviceShaderFloat16Int8Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
                pClientDescriptorIndexingFeatures = (VkPhysicalDeviceDescriptorIndexingFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
                pClientScalarBlockLayoutFeatures = (VkPhysicalDeviceScalarBlockLayoutFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
                pClientImagelessFramebufferFeatures = (VkPhysicalDeviceImagelessFramebufferFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
                pClientUniformBufferStandardLayoutFeatures = (VkPhysicalDeviceUniformBufferStandardLayoutFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
                pClientShaderSubgroupExtendedTypesFeatures = (VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
                pClientSeparateDepthStencilLayoutsFeatures = (VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
                pClientHostQueryResetFeatures = (VkPhysicalDeviceHostQueryResetFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
                pClientTimelineSemaphoreFeatures = (VkPhysicalDeviceTimelineSemaphoreFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
                pClientBufferDeviceAddressFeatures = (VkPhysicalDeviceBufferDeviceAddressFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
                pClientVulkanMemoryModelFeatures = (VkPhysicalDeviceVulkanMemoryModelFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                pClientPhysicalDevice13Features = (VkPhysicalDeviceVulkan13Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
                pClientDynamicRenderingFeatures = (VkPhysicalDeviceDynamicRenderingFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
                pClientImageRobustnessFeatures = (VkPhysicalDeviceImageRobustnessFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
                pClientInlineUniformBlockFeatures = (VkPhysicalDeviceInlineUniformBlockFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
                pClientMaintenance4Features = (VkPhysicalDeviceMaintenance4Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
                pClientPipelineCreationCacheControlFeatures = (VkPhysicalDevicePipelineCreationCacheControlFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
                pClientPrivateDataFeatures = (VkPhysicalDevicePrivateDataFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
                pClientShaderDemoteToHelperInvocationFeatures = (VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
                pClientShaderIntegerDotProductFeatures = (VkPhysicalDeviceShaderIntegerDotProductFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
                pClientShaderTerminateInvocationFeatures = (VkPhysicalDeviceShaderTerminateInvocationFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
                pClientSubgroupSizeControlFeatures = (VkPhysicalDeviceSubgroupSizeControlFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
                pClientSynchronization2Features = (VkPhysicalDeviceSynchronization2Features*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES:
                pClientTextureCompressionASTCHDRFeatures = (VkPhysicalDeviceTextureCompressionASTCHDRFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
                pClientZeroInitializeWorkgroupMemoryFeatures = (VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures*)pClientFeaturesChain;
                break;

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV:
                pClientOpticalFlowFeaturesNV = (VkPhysicalDeviceOpticalFlowFeaturesNV*)pClientFeaturesChain;
                break;

            default:
                break;
            }

            pClientFeaturesChain = pClientFeaturesChain->pNext;
        }

        if (pClient8BitStorageFeatures != NULL
            || pClientShaderAtomicInt64Features != NULL
            || pClientShaderFloat16Int8Features != NULL
            || pClientDescriptorIndexingFeatures != NULL
            || pClientScalarBlockLayoutFeatures != NULL
            || pClientImagelessFramebufferFeatures != NULL
            || pClientUniformBufferStandardLayoutFeatures != NULL
            || pClientShaderSubgroupExtendedTypesFeatures != NULL
            || pClientSeparateDepthStencilLayoutsFeatures != NULL
            || pClientHostQueryResetFeatures != NULL
            || pClientTimelineSemaphoreFeatures != NULL
            || pClientBufferDeviceAddressFeatures != NULL
            || pClientVulkanMemoryModelFeatures != NULL)
        {
            if (pClientPhysicalDevice12Features != NULL)
            {
                SL_LOG_ERROR("As per VK spec - VUID-VkDeviceCreateInfo-pNext-02830, client VkDeviceCreateInfo object cannot contain pointer to VkPhysicalDeviceVulkan12Features object, \
                              if it already contains features from that struct chained individually!");
            }

            if (requiredSLPhysicalDevice12Features.shaderFloat16 && supportedPhysicalDevice12Features.shaderFloat16)
            {
                if (pClientShaderFloat16Int8Features == NULL)
                {
                    shaderFloat16Int8Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
                    shaderFloat16Int8Features.shaderFloat16 = true;
                    shaderFloat16Int8Features.pNext = (void*)(createInfo.pNext);
                    createInfo.pNext = &shaderFloat16Int8Features;
                }
                else
                {
                    // feature already added by host - enable it
                    pClientShaderFloat16Int8Features->shaderFloat16 = true;
                }
            }

            if (requiredSLPhysicalDevice12Features.descriptorIndexing && supportedPhysicalDevice12Features.descriptorIndexing)
            {
                requiredSLDeviceExtensionNames.emplace(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            }

            if (requiredSLPhysicalDevice12Features.timelineSemaphore && supportedPhysicalDevice12Features.timelineSemaphore)
            {
                if (pClientTimelineSemaphoreFeatures == NULL)
                {
                    timelineSemaphoreFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
                    timelineSemaphoreFeatures.timelineSemaphore = true;
                    timelineSemaphoreFeatures.pNext = (void*)(createInfo.pNext);
                    createInfo.pNext = &timelineSemaphoreFeatures;
                }
                else
                {
                    pClientTimelineSemaphoreFeatures->timelineSemaphore = true;
                }

                requiredSLDeviceExtensionNames.emplace(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            }

            if (requiredSLPhysicalDevice12Features.bufferDeviceAddress && supportedPhysicalDevice12Features.bufferDeviceAddress)
            {
                if (pClientBufferDeviceAddressFeatures == NULL)
                {
                    bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                    bufferDeviceAddressFeatures.bufferDeviceAddress = true;
                    bufferDeviceAddressFeatures.pNext = (void*)(createInfo.pNext);
                    createInfo.pNext = &bufferDeviceAddressFeatures;
                }
                else
                {
                    pClientBufferDeviceAddressFeatures->bufferDeviceAddress = true;
                }

                requiredSLDeviceExtensionNames.emplace(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            }
        }
        else
        {
            if (pClientPhysicalDevice12Features == NULL && !requiredPhysicalDevice12FeatureNames.empty())
            {
                // if host hasn't added VK 1.2 features, add it to the host if required
                requiredSLPhysicalDevice12Features.pNext = (void*)createInfo.pNext;
                createInfo.pNext = pClientPhysicalDevice12Features = &requiredSLPhysicalDevice12Features;
            }
        }

        if (pClientDynamicRenderingFeatures != NULL
            || pClientImageRobustnessFeatures != NULL
            || pClientInlineUniformBlockFeatures != NULL
            || pClientMaintenance4Features != NULL
            || pClientPipelineCreationCacheControlFeatures != NULL
            || pClientPrivateDataFeatures != NULL
            || pClientShaderDemoteToHelperInvocationFeatures != NULL
            || pClientShaderIntegerDotProductFeatures != NULL
            || pClientShaderTerminateInvocationFeatures != NULL
            || pClientSubgroupSizeControlFeatures != NULL
            || pClientSynchronization2Features != NULL
            || pClientTextureCompressionASTCHDRFeatures != NULL
            || pClientZeroInitializeWorkgroupMemoryFeatures != NULL)
        {
            if (pClientPhysicalDevice13Features != NULL)
            {
                SL_LOG_ERROR("As per VK spec - VUID-VkDeviceCreateInfo-pNext-02830, client VkDeviceCreateInfo cannot contain pointer to VkPhysicalDeviceVulkan13Features object, \
                              if it already contains features from that struct chained individually!");
            }

            if ((requiredSLPhysicalDevice13Features.synchronization2 || s_vk.nativeOpticalFlowHWSupport) && supportedPhysicalDevice13Features.synchronization2)
            {
                if (pClientSynchronization2Features == NULL)
                {
                    synchronization2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
                    synchronization2Features.synchronization2 = true;
                    synchronization2Features.pNext = (void*)(createInfo.pNext);
                    createInfo.pNext = &synchronization2Features;
                }
                else
                {
                    // feature already added by host - enable it
                    pClientSynchronization2Features->synchronization2 = true;
                }

                requiredSLDeviceExtensionNames.emplace(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            }
        }
        else
        {
            if (pClientPhysicalDevice13Features == NULL && !requiredPhysicalDevice13FeatureNames.empty())
            {
                // if host hasn't added VK 1.2 features, add it to the host if required
                requiredSLPhysicalDevice13Features.pNext = (void*)createInfo.pNext;
                createInfo.pNext = pClientPhysicalDevice13Features = &requiredSLPhysicalDevice13Features;
            }
        }

        VkPhysicalDeviceOpticalFlowFeaturesNV opticalFlowFeaturesNV{};
        if ((requiredSLOpticalFlowNVFeatures.opticalFlow || s_vk.nativeOpticalFlowHWSupport) && supportedOpticalFlowFeaturesNV.opticalFlow)
        {
            s_vk.nativeOpticalFlowHWSupport = true;

            if (pClientOpticalFlowFeaturesNV == NULL)
            {
                opticalFlowFeaturesNV.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
                opticalFlowFeaturesNV.opticalFlow = true;
                opticalFlowFeaturesNV.pNext = (void*)createInfo.pNext;
                createInfo.pNext = &opticalFlowFeaturesNV;
            }
            else
            {
                // feature already added by host - enable it
                pClientOpticalFlowFeaturesNV->opticalFlow = true;
            }

            requiredSLDeviceExtensionNames.emplace(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
        }

        VkBaseOutStructure* pPhysicalDevice12FeaturesToMerge{};
        if (pClientPhysicalDevice12Features != NULL)
        {
            if (pClientPhysicalDevice12Features != (&requiredSLPhysicalDevice12Features))
            {
                // Host has already added 1.2 features. merge SL-required features.
                pPhysicalDevice12FeaturesToMerge = (VkBaseOutStructure*)(&requiredSLPhysicalDevice12Features);
            }

            sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures((VkBaseOutStructure*)pClientPhysicalDevice12Features, pPhysicalDevice12FeaturesToMerge, (VkBaseOutStructure*)(&supportedPhysicalDevice12Features));
        }

        VkBaseOutStructure* pPhysicalDevice13FeaturesToMerge{};
        if (pClientPhysicalDevice13Features != NULL)
        {
            if (pClientPhysicalDevice13Features != (&requiredSLPhysicalDevice13Features))
            {
                // Host has already added 1.3 features. merge SL-required features.
                pPhysicalDevice13FeaturesToMerge = (VkBaseOutStructure*)(&requiredSLPhysicalDevice13Features);
            }

            sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures((VkBaseOutStructure*)pClientPhysicalDevice13Features, pPhysicalDevice13FeaturesToMerge, (VkBaseOutStructure*)(&supportedPhysicalDevice13Features));
        }

        uint32_t deviceExtensionCount{};
        VK_CHECK_RI(vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, NULL));
        if (deviceExtensionCount == 0)
        {
            SL_LOG_ERROR("No supported device extensions enumerated!");
        }
        std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
        VK_CHECK_RI(vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, availableDeviceExtensions.data()));

        std::unordered_set<std::string> unsupportedDeviceExtensionNames(requiredSLDeviceExtensionNames);
        for (const auto& ext : availableDeviceExtensions)
        {
            unsupportedDeviceExtensionNames.erase(ext.extensionName);
        }
        if (!unsupportedDeviceExtensionNames.empty())
        {
            for (const auto& ext : unsupportedDeviceExtensionNames)
            {
                SL_LOG_ERROR("Required device extension %s unsupported!", ext.c_str());
                requiredSLDeviceExtensionNames.erase(ext);
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }

        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++)
        {
            requiredSLDeviceExtensionNames.insert(createInfo.ppEnabledExtensionNames[i]);
        }

        for (auto& ext : requiredSLDeviceExtensionNames)
        {
            if (ext == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
            {
                if (auto search = requiredSLDeviceExtensionNames.find(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME); search != requiredSLDeviceExtensionNames.end())
                {
                    requiredSLDeviceExtensionNames.erase(search);
                    SL_LOG_INFO("As per VK spec - VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328, both extensions - VK_KHR_buffer_device_address and VK_EXT_buffer_device_address \
                        cannot be enabled at the same time. Removed older extension VK_EXT_buffer_device_address.");
                }
            }
            SL_LOG_INFO("Device extension '%s' requested by a plugin(s) added.", ext.c_str());
        }

        std::vector<const char*> extensions;
        extensions.reserve(requiredSLDeviceExtensionNames.size());
        for (auto& e : requiredSLDeviceExtensionNames)
        {
            extensions.emplace_back(e.c_str());
        }

        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

        auto& dt = s_vk.dispatchInstanceMap[s_vk.instance];

        // Queue family properties, used for setting up requested queues upon device creation
        uint32_t queueFamilyCount;
        s_idt.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilyProperties;
        queueFamilyProperties.resize(queueFamilyCount);
        s_idt.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

        s_vk.graphicsQueueFamily = 0;
        s_vk.computeQueueFamily = 0;
        std::unordered_map<uint32_t, VkQueueFlags> graphicsComputeQueueFamilyIndex{};
        for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++)
        {
            if (!s_vk.nativeOpticalFlowHWSupport || i != s_vk.opticalFlowQueueFamily)
            {
                if ((queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                {
                    SL_LOG_VERBOSE("Found Vulkan graphics queue family at index %u - max queues allowed %u", i, queueFamilyProperties[i].queueCount);
                    s_vk.graphicsQueueFamily = i;
                    graphicsComputeQueueFamilyIndex[i] = VK_QUEUE_GRAPHICS_BIT;
                }
                else if ((queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
                {
                    SL_LOG_VERBOSE("Found Vulkan compute queue family at index %u - max queues allowed %u", i, queueFamilyProperties[i].queueCount);
                    s_vk.computeQueueFamily = i;
                    graphicsComputeQueueFamilyIndex[i] = VK_QUEUE_COMPUTE_BIT;
                }
            }
        }

        // Check and add extra graphics and compute queues for SL workloads
        s_vk.computeQueueIndex = 0;
        s_vk.graphicsQueueIndex = 0;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        const float defaultQueuePriority = 0.0f;
        std::vector<float> computeQueuePriorities( extraComputeQueues, defaultQueuePriority );
        std::vector<float> graphicsQueuePriorities(extraGraphicsQueues, defaultQueuePriority);
        std::vector<float> opticalFlowQueuePriorities(extraOpticalFlowQueues, defaultQueuePriority);
        for (uint32_t i = 0; i < createInfo.queueCreateInfoCount; i++)
        {
            queueCreateInfos.push_back(createInfo.pQueueCreateInfos[i]);
            if (auto search = graphicsComputeQueueFamilyIndex.find(queueCreateInfos.back().queueFamilyIndex); search != graphicsComputeQueueFamilyIndex.end())
            {
                s_vk.hostGraphicsComputeQueueInfo.emplace_back(QueueVkInfo{ search->second, search->first, {}, queueCreateInfos.back().flags, queueCreateInfos.back().queueCount });
            }
            if (extraComputeQueues != 0 && createInfo.pQueueCreateInfos[i].queueFamilyIndex == s_vk.computeQueueFamily)
            {
                if (queueFamilyProperties[s_vk.computeQueueFamily].queueCount < queueCreateInfos.back().queueCount + extraComputeQueues)
                {
                    SL_LOG_WARN("SL feature(s) requiring more compute queues than available on this device");
                    continue;
                }
                s_vk.computeQueueCreateFlags = queueCreateInfos.back().flags;
                s_vk.computeQueueIndex += queueCreateInfos.back().queueCount;
                computeQueuePriorities.insert(computeQueuePriorities.begin(), queueCreateInfos.back().pQueuePriorities, queueCreateInfos.back().pQueuePriorities + queueCreateInfos.back().queueCount);
                queueCreateInfos.back().pQueuePriorities = computeQueuePriorities.data();
                queueCreateInfos.back().queueCount += extraComputeQueues; // defaults to 0 unless requested otherwise by plugin(s)
                extraComputeQueues = 0;
            }
            if (extraGraphicsQueues != 0 && createInfo.pQueueCreateInfos[i].queueFamilyIndex == s_vk.graphicsQueueFamily)
            {
                if (queueFamilyProperties[s_vk.graphicsQueueFamily].queueCount < queueCreateInfos.back().queueCount + extraGraphicsQueues)
                {
                    SL_LOG_WARN("SL feature(s) requiring more graphics queues than available on this device");
                    continue;
                }
                s_vk.graphicsQueueCreateFlags = queueCreateInfos.back().flags;
                s_vk.graphicsQueueIndex += queueCreateInfos.back().queueCount;
                graphicsQueuePriorities.insert(graphicsQueuePriorities.begin(), queueCreateInfos.back().pQueuePriorities, queueCreateInfos.back().pQueuePriorities + queueCreateInfos.back().queueCount);
                queueCreateInfos.back().pQueuePriorities = graphicsQueuePriorities.data();
                queueCreateInfos.back().queueCount += extraGraphicsQueues; // defaults to 0 unless requested otherwise by plugin(s)
                extraGraphicsQueues = 0;
            }
        }

        VkDeviceQueueCreateInfo queueInfo{};

        if (extraComputeQueues > 0 && s_vk.computeQueueIndex == 0 && queueFamilyProperties[s_vk.computeQueueFamily].queueCount >= extraComputeQueues)
        {
            // We have to add compute queue(s) explicitly since host has none
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = s_vk.computeQueueFamily;
            queueInfo.flags = s_vk.computeQueueCreateFlags;
            queueInfo.queueCount = extraComputeQueues;
            queueInfo.pQueuePriorities = computeQueuePriorities.data();
            queueCreateInfos.push_back(queueInfo);
            extraComputeQueues = 0;
        }

        if (s_vk.nativeOpticalFlowHWSupport && extraOpticalFlowQueues > 0 && queueFamilyProperties[s_vk.opticalFlowQueueFamily].queueCount >= extraOpticalFlowQueues)
        {
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = s_vk.opticalFlowQueueFamily;
            queueInfo.flags = s_vk.opticalFlowQueueCreateFlags;
            queueInfo.queueCount = extraOpticalFlowQueues;
            queueInfo.pQueuePriorities = opticalFlowQueuePriorities.data();
            queueCreateInfos.push_back(queueInfo);
            extraOpticalFlowQueues = 0;
        }

        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();

        auto trampoline = (PFN_vkCreateDevice)GetProcAddress(lib, "vkCreateDevice");
        auto res = trampoline(physicalDevice, &createInfo, pAllocator, pDevice);

        if (res != VK_SUCCESS)
        {
            SL_LOG_ERROR( "vkCreateDevice failed");
            return res;
        }
        s_vk.instance = s_vk.instanceDeviceMap[physicalDevice];
        s_vk.mapVulkanInstanceAPI(s_vk.instance);
        s_idt = s_vk.dispatchInstanceMap[s_vk.instance];

        s_vk.device = *pDevice;
        s_vk.mapVulkanDeviceAPI(*pDevice);

        sl::param::getInterface()->set(sl::param::global::kVulkanTable, &s_vk);

        s_ddt = s_vk.dispatchDeviceMap[s_vk.device];

        pluginManager->setVulkanDevice(physicalDevice, *pDevice, s_vk.instance);
        pluginManager->initializePlugins();

        return res;
    }

    void VKAPI_CALL vkDestroyInstance(VkInstance Instance, const VkAllocationCallbacks* Allocator)
    {
        s_idt.DestroyInstance(Instance, Allocator);
        auto it = s_vk.instanceDeviceMap.begin();
        while (it != s_vk.instanceDeviceMap.end())
        {
            if ((*it).second == Instance)
            {
                it = s_vk.instanceDeviceMap.erase(it);
            }
            else
            {
                it++;
            }
        }
    }

    VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance Instance, uint32_t* PhysicalDeviceCount, VkPhysicalDevice* PhysicalDevices)
    {
        VkResult Result = s_idt.EnumeratePhysicalDevices(Instance, PhysicalDeviceCount, PhysicalDevices);
        if (PhysicalDevices)
        {
            auto i = *PhysicalDeviceCount;
            while (i--)
            {
                s_vk.instanceDeviceMap[PhysicalDevices[i]] = Instance;
            }
        }
        return Result;
    }

    void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures* Features)
    {
        s_idt.GetPhysicalDeviceFeatures(PhysicalDevice, Features);
    }

    void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkFormatProperties* FormatProperties)
    {
        s_idt.GetPhysicalDeviceFormatProperties(PhysicalDevice, Format, FormatProperties);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type, VkImageTiling Tiling, VkImageUsageFlags Usage, VkImageCreateFlags Flags, VkImageFormatProperties* pImageFormatProperties)
    {
        return s_idt.GetPhysicalDeviceImageFormatProperties(PhysicalDevice, Format, Type, Tiling, Usage, Flags, pImageFormatProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties* Properties)
    {
        s_idt.GetPhysicalDeviceProperties(PhysicalDevice, Properties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice PhysicalDevice, uint32_t* QueueFamilyPropertyCount, VkQueueFamilyProperties* QueueFamilyProperties)
    {
        s_idt.GetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, QueueFamilyPropertyCount, QueueFamilyProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties* MemoryProperties)
    {
        s_idt.GetPhysicalDeviceMemoryProperties(PhysicalDevice, MemoryProperties);
    }

    void VKAPI_CALL vkDestroyDevice(VkDevice Device, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDevice(Device, Allocator);
    }

    VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice PhysicalDevice, const char* LayerName, uint32_t* PropertyCount, VkExtensionProperties* Properties)
    {
        return s_idt.EnumerateDeviceExtensionProperties(PhysicalDevice, LayerName, PropertyCount, Properties);
    }

    VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice PhysicalDevice, uint32_t* PropertyCount, VkLayerProperties* Properties)
    {
        return s_idt.EnumerateDeviceLayerProperties(PhysicalDevice, PropertyCount, Properties);
    }

    void VKAPI_CALL vkGetDeviceQueue(VkDevice Device, uint32_t QueueFamilyIndex, uint32_t QueueIndex, VkQueue* Queue)
    {
        s_ddt.GetDeviceQueue(Device, QueueFamilyIndex, QueueIndex, Queue);
    }

    VkResult VKAPI_CALL vkQueueSubmit(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence)
    {
        return s_ddt.QueueSubmit(Queue, SubmitCount, Submits, Fence);
    }
    VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue Queue)
    {
        return s_ddt.QueueWaitIdle(Queue);
    }

    VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice Device)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_DeviceWaitIdle);
        bool skip = false;
        VkResult result = VK_SUCCESS;
        for (auto [hook, feature] : hooks)
        {
            result = ((sl::PFunVkDeviceWaitIdleBefore*)hook)(Device, skip);
            // report error on first fail
            if (result != VK_SUCCESS)
            {
                return result;
            }
        }

        if (!skip)
        {
            result = s_ddt.DeviceWaitIdle(Device);
        }
        return result;
    }

    VkResult VKAPI_CALL vkAllocateMemory(VkDevice Device, const VkMemoryAllocateInfo* AllocateInfo, const VkAllocationCallbacks* Allocator, VkDeviceMemory* Memory)
    {
        return s_ddt.AllocateMemory(Device, AllocateInfo, Allocator, Memory);
    }

    void VKAPI_CALL vkFreeMemory(VkDevice Device, VkDeviceMemory Memory, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.FreeMemory(Device, Memory, Allocator);
    }

    VkResult VKAPI_CALL vkMapMemory(VkDevice Device, VkDeviceMemory Memory, VkDeviceSize Offset, VkDeviceSize Size, VkMemoryMapFlags Flags, void** Data)
    {
        return s_ddt.MapMemory(Device, Memory, Offset, Size, Flags, Data);
    }

    void VKAPI_CALL vkUnmapMemory(VkDevice Device, VkDeviceMemory Memory)
    {
        s_ddt.UnmapMemory(Device, Memory);
    }

    VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice Device, uint32_t MemoryRangeCount, const VkMappedMemoryRange* MemoryRanges)
    {
        return s_ddt.FlushMappedMemoryRanges(Device, MemoryRangeCount, MemoryRanges);
    }

    VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice Device, uint32_t MemoryRangeCount, const VkMappedMemoryRange* MemoryRanges)
    {
        return s_ddt.InvalidateMappedMemoryRanges(Device, MemoryRangeCount, MemoryRanges);
    }

    void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice Device, VkDeviceMemory Memory, VkDeviceSize* pCommittedMemoryInBytes)
    {
        s_ddt.GetDeviceMemoryCommitment(Device, Memory, pCommittedMemoryInBytes);
    }

    VkResult VKAPI_CALL vkBindBufferMemory(VkDevice Device, VkBuffer Buffer, VkDeviceMemory Memory, VkDeviceSize MemoryOffset)
    {
        return s_ddt.BindBufferMemory(Device, Buffer, Memory, MemoryOffset);
    }

    VkResult VKAPI_CALL vkBindImageMemory(VkDevice Device, VkImage Image, VkDeviceMemory Memory, VkDeviceSize MemoryOffset)
    {
        return s_ddt.BindImageMemory(Device, Image, Memory, MemoryOffset);
    }

    void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice Device, VkBuffer Buffer, VkMemoryRequirements* MemoryRequirements)
    {
        s_ddt.GetBufferMemoryRequirements(Device, Buffer, MemoryRequirements);
    }

    void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice Device, VkImage Image, VkMemoryRequirements* MemoryRequirements)
    {
        s_ddt.GetImageMemoryRequirements(Device, Image, MemoryRequirements);
    }

    void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice Device, VkImage Image, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements* pSparseMemoryRequirements)
    {
        s_ddt.GetImageSparseMemoryRequirements(Device, Image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
    }

    void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type, VkSampleCountFlagBits Samples, VkImageUsageFlags Usage, VkImageTiling Tiling, uint32_t* pPropertyCount, VkSparseImageFormatProperties* pProperties)
    {
        s_idt.GetPhysicalDeviceSparseImageFormatProperties(PhysicalDevice, Format, Type, Samples, Usage, Tiling, pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkQueueBindSparse(VkQueue Queue, uint32_t BindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence Fence)
    {
        return s_ddt.QueueBindSparse(Queue, BindInfoCount, pBindInfo, Fence);
    }

    VkResult VKAPI_CALL vkCreateFence(VkDevice Device, const VkFenceCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkFence* Fence)
    {
        return s_ddt.CreateFence(Device, CreateInfo, Allocator, Fence);
    }

    void VKAPI_CALL vkDestroyFence(VkDevice Device, VkFence Fence, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyFence(Device, Fence, Allocator);
    }

    VkResult VKAPI_CALL vkResetFences(VkDevice Device, uint32_t FenceCount, const VkFence* Fences)
    {
        return s_ddt.ResetFences(Device, FenceCount, Fences);
    }

    VkResult VKAPI_CALL vkGetFenceStatus(VkDevice Device, VkFence Fence)
    {
        return s_ddt.GetFenceStatus(Device, Fence);
    }

    VkResult VKAPI_CALL vkWaitForFences(VkDevice Device, uint32_t FenceCount, const VkFence* Fences, VkBool32 bWaitAll, uint64_t Timeout)
    {
        return s_ddt.WaitForFences(Device, FenceCount, Fences, bWaitAll, Timeout);
    }

    VkResult VKAPI_CALL vkCreateSemaphore(VkDevice Device, const VkSemaphoreCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkSemaphore* Semaphore)
    {
        return s_ddt.CreateSemaphore(Device, CreateInfo, Allocator, Semaphore);
    }


    void VKAPI_CALL vkDestroySemaphore(VkDevice Device, VkSemaphore Semaphore, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroySemaphore(Device, Semaphore, Allocator);
    }

    VkResult VKAPI_CALL vkCreateEvent(VkDevice Device, const VkEventCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkEvent* Event)
    {
        return s_ddt.CreateEvent(Device, CreateInfo, Allocator, Event);
    }

    void VKAPI_CALL vkDestroyEvent(VkDevice Device, VkEvent Event, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyEvent(Device, Event, Allocator);
    }

    VkResult VKAPI_CALL vkGetEventStatus(VkDevice Device, VkEvent Event)
    {
        return s_ddt.GetEventStatus(Device, Event);
    }

    VkResult VKAPI_CALL vkSetEvent(VkDevice Device, VkEvent Event)
    {
        return s_ddt.SetEvent(Device, Event);
    }

    VkResult VKAPI_CALL vkResetEvent(VkDevice Device, VkEvent Event)
    {
        return s_ddt.ResetEvent(Device, Event);
    }

    VkResult VKAPI_CALL vkCreateQueryPool(VkDevice Device, const VkQueryPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkQueryPool* QueryPool)
    {
        return s_ddt.CreateQueryPool(Device, CreateInfo, Allocator, QueryPool);
    }

    void VKAPI_CALL vkDestroyQueryPool(VkDevice Device, VkQueryPool QueryPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyQueryPool(Device, QueryPool, Allocator);
    }

    VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice Device, VkQueryPool QueryPool,
        uint32_t FirstQuery, uint32_t QueryCount, size_t DataSize, void* Data, VkDeviceSize Stride, VkQueryResultFlags Flags)
    {
        return s_ddt.GetQueryPoolResults(Device, QueryPool, FirstQuery, QueryCount, DataSize, Data, Stride, Flags);
    }

    VkResult VKAPI_CALL vkCreateBuffer(VkDevice Device, const VkBufferCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkBuffer* Buffer)
    {
        return s_ddt.CreateBuffer(Device, CreateInfo, Allocator, Buffer);
    }

    void VKAPI_CALL vkDestroyBuffer(VkDevice Device, VkBuffer Buffer, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyBuffer(Device, Buffer, Allocator);
    }

    VkResult VKAPI_CALL vkCreateBufferView(VkDevice Device, const VkBufferViewCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkBufferView* View)
    {
        return s_ddt.CreateBufferView(Device, CreateInfo, Allocator, View);
    }

    void VKAPI_CALL vkDestroyBufferView(VkDevice Device, VkBufferView BufferView, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyBufferView(Device, BufferView, Allocator);
    }

    VkResult VKAPI_CALL vkCreateImage(VkDevice Device, const VkImageCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkImage* Image)
    {
        return s_ddt.CreateImage(Device, CreateInfo, Allocator, Image);
    }

    void VKAPI_CALL vkDestroyImage(VkDevice Device, VkImage Image, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyImage(Device, Image, Allocator);
    }

    void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice Device, VkImage Image, const VkImageSubresource* Subresource, VkSubresourceLayout* Layout)
    {
        s_ddt.GetImageSubresourceLayout(Device, Image, Subresource, Layout);
    }

    VkResult VKAPI_CALL vkCreateImageView(VkDevice Device, const VkImageViewCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkImageView* View)
    {
        return s_ddt.CreateImageView(Device, CreateInfo, Allocator, View);
    }

    void VKAPI_CALL vkDestroyImageView(VkDevice Device, VkImageView ImageView, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyImageView(Device, ImageView, Allocator);
    }

    VkResult VKAPI_CALL vkCreateShaderModule(VkDevice Device, const VkShaderModuleCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkShaderModule* ShaderModule)
    {
        return s_ddt.CreateShaderModule(Device, CreateInfo, Allocator, ShaderModule);
    }

    void VKAPI_CALL vkDestroyShaderModule(VkDevice Device, VkShaderModule ShaderModule, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyShaderModule(Device, ShaderModule, Allocator);
    }

    VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice Device, const VkPipelineCacheCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkPipelineCache* PipelineCache)
    {
        return s_ddt.CreatePipelineCache(Device, CreateInfo, Allocator, PipelineCache);
    }

    void VKAPI_CALL vkDestroyPipelineCache(VkDevice Device, VkPipelineCache PipelineCache, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipelineCache(Device, PipelineCache, Allocator);
    }

    VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice Device, VkPipelineCache PipelineCache, size_t* DataSize, void* Data)
    {
        return s_ddt.GetPipelineCacheData(Device, PipelineCache, DataSize, Data);
    }

    VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice Device, VkPipelineCache DestCache, uint32_t SourceCacheCount, const VkPipelineCache* SrcCaches)
    {
        return s_ddt.MergePipelineCaches(Device, DestCache, SourceCacheCount, SrcCaches);
    }

    VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice Device, VkPipelineCache PipelineCache, uint32_t CreateInfoCount, const VkGraphicsPipelineCreateInfo* CreateInfos, const VkAllocationCallbacks* Allocator, VkPipeline* Pipelines)
    {
        return s_ddt.CreateGraphicsPipelines(Device, PipelineCache, CreateInfoCount, CreateInfos, Allocator, Pipelines);
    }

    VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice Device, VkPipelineCache PipelineCache, uint32_t CreateInfoCount, const VkComputePipelineCreateInfo* CreateInfos, const VkAllocationCallbacks* Allocator, VkPipeline* Pipelines)
    {
        return s_ddt.CreateComputePipelines(Device, PipelineCache, CreateInfoCount, CreateInfos, Allocator, Pipelines);
    }

    void VKAPI_CALL vkDestroyPipeline(VkDevice Device, VkPipeline Pipeline, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipeline(Device, Pipeline, Allocator);
    }

    VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice Device, const VkPipelineLayoutCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkPipelineLayout* PipelineLayout)
    {
        return s_ddt.CreatePipelineLayout(Device, CreateInfo, Allocator, PipelineLayout);
    }

    void VKAPI_CALL vkDestroyPipelineLayout(VkDevice Device, VkPipelineLayout PipelineLayout, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipelineLayout(Device, PipelineLayout, Allocator);
    }

    VkResult VKAPI_CALL vkCreateSampler(VkDevice Device, const VkSamplerCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkSampler* Sampler)
    {
        return s_ddt.CreateSampler(Device, CreateInfo, Allocator, Sampler);
    }

    void VKAPI_CALL vkDestroySampler(VkDevice Device, VkSampler Sampler, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroySampler(Device, Sampler, Allocator);
    }

    VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice Device, const VkDescriptorSetLayoutCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkDescriptorSetLayout* SetLayout)
    {
        return s_ddt.CreateDescriptorSetLayout(Device, CreateInfo, Allocator, SetLayout);
    }

    void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice Device, VkDescriptorSetLayout DescriptorSetLayout, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDescriptorSetLayout(Device, DescriptorSetLayout, Allocator);
    }

    VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice Device, const VkDescriptorPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkDescriptorPool* DescriptorPool)
    {
        return s_ddt.CreateDescriptorPool(Device, CreateInfo, Allocator, DescriptorPool);
    }

    void VKAPI_CALL vkDestroyDescriptorPool(VkDevice Device, VkDescriptorPool DescriptorPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDescriptorPool(Device, DescriptorPool, Allocator);
    }

    VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice Device, VkDescriptorPool DescriptorPool, VkDescriptorPoolResetFlags Flags)
    {
        return s_ddt.ResetDescriptorPool(Device, DescriptorPool, Flags);
    }

    VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice Device, const VkDescriptorSetAllocateInfo* AllocateInfo, VkDescriptorSet* DescriptorSets)
    {
        return s_ddt.AllocateDescriptorSets(Device, AllocateInfo, DescriptorSets);
    }

    VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice Device, VkDescriptorPool DescriptorPool, uint32_t DescriptorSetCount, const VkDescriptorSet* DescriptorSets)
    {
        return s_ddt.FreeDescriptorSets(Device, DescriptorPool, DescriptorSetCount, DescriptorSets);
    }

    void VKAPI_CALL vkUpdateDescriptorSets(VkDevice Device, uint32_t DescriptorWriteCount, const VkWriteDescriptorSet* DescriptorWrites, uint32_t DescriptorCopyCount, const VkCopyDescriptorSet* DescriptorCopies)
    {
        s_ddt.UpdateDescriptorSets(Device, DescriptorWriteCount, DescriptorWrites, DescriptorCopyCount, DescriptorCopies);
    }

    VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice Device, const VkFramebufferCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkFramebuffer* Framebuffer)
    {
        return s_ddt.CreateFramebuffer(Device, CreateInfo, Allocator, Framebuffer);
    }

    void VKAPI_CALL vkDestroyFramebuffer(VkDevice Device, VkFramebuffer Framebuffer, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyFramebuffer(Device, Framebuffer, Allocator);
    }

    VkResult VKAPI_CALL vkCreateRenderPass(VkDevice Device, const VkRenderPassCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkRenderPass* RenderPass)
    {
        return s_ddt.CreateRenderPass(Device, CreateInfo, Allocator, RenderPass);
    }

    void VKAPI_CALL vkDestroyRenderPass(VkDevice Device, VkRenderPass RenderPass, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyRenderPass(Device, RenderPass, Allocator);
    }

    void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice Device, VkRenderPass RenderPass, VkExtent2D* pGranularity)
    {
        s_ddt.GetRenderAreaGranularity(Device, RenderPass, pGranularity);
    }

    VkResult VKAPI_CALL vkCreateCommandPool(VkDevice Device, const VkCommandPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkCommandPool* CommandPool)
    {
        return s_ddt.CreateCommandPool(Device, CreateInfo, Allocator, CommandPool);
    }

    void VKAPI_CALL vkDestroyCommandPool(VkDevice Device, VkCommandPool CommandPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyCommandPool(Device, CommandPool, Allocator);
    }

    VkResult VKAPI_CALL vkResetCommandPool(VkDevice Device, VkCommandPool CommandPool, VkCommandPoolResetFlags Flags)
    {
        return s_ddt.ResetCommandPool(Device, CommandPool, Flags);
    }

    VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice Device, const VkCommandBufferAllocateInfo* AllocateInfo, VkCommandBuffer* CommandBuffers)
    {
        return s_ddt.AllocateCommandBuffers(Device, AllocateInfo, CommandBuffers);
    }

    void VKAPI_CALL vkFreeCommandBuffers(VkDevice Device, VkCommandPool CommandPool, uint32_t CommandBufferCount, const VkCommandBuffer* CommandBuffers)
    {
        s_ddt.FreeCommandBuffers(Device, CommandPool, CommandBufferCount, CommandBuffers);
    }

    VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer CommandBuffer, const VkCommandBufferBeginInfo* BeginInfo)
    {
        auto res = s_ddt.BeginCommandBuffer(CommandBuffer, BeginInfo);

        return res;
    }

    VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer CommandBuffer)
    {
        return s_ddt.EndCommandBuffer(CommandBuffer);
    }

    VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer CommandBuffer, VkCommandBufferResetFlags Flags)
    {
        return s_ddt.ResetCommandBuffer(CommandBuffer, Flags);
    }

    void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipeline Pipeline)
    {
        s_ddt.CmdBindPipeline(CommandBuffer, PipelineBindPoint, Pipeline);
    }

    void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer CommandBuffer, uint32_t FirstViewport, uint32_t ViewportCount, const VkViewport* Viewports)
    {
        s_ddt.CmdSetViewport(CommandBuffer, FirstViewport, ViewportCount, Viewports);
    }

    void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer CommandBuffer, uint32_t FirstScissor, uint32_t ScissorCount, const VkRect2D* Scissors)
    {
        s_ddt.CmdSetScissor(CommandBuffer, FirstScissor, ScissorCount, Scissors);
    }

    void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer CommandBuffer, float LineWidth)
    {
        s_ddt.CmdSetLineWidth(CommandBuffer, LineWidth);
    }

    void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer CommandBuffer, float DepthBiasConstantFactor, float DepthBiasClamp, float DepthBiasSlopeFactor)
    {
        s_ddt.CmdSetDepthBias(CommandBuffer, DepthBiasConstantFactor, DepthBiasClamp, DepthBiasSlopeFactor);
    }

    void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer CommandBuffer, const float BlendConstants[4])
    {
        s_ddt.CmdSetBlendConstants(CommandBuffer, BlendConstants);
    }

    void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer CommandBuffer, float MinDepthBounds, float MaxDepthBounds)
    {
        s_ddt.CmdSetDepthBounds(CommandBuffer, MinDepthBounds, MaxDepthBounds);
    }

    void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t CompareMask)
    {
        s_ddt.CmdSetStencilCompareMask(CommandBuffer, FaceMask, CompareMask);
    }

    void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t WriteMask)
    {
        s_ddt.CmdSetStencilWriteMask(CommandBuffer, FaceMask, WriteMask);
    }

    void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t Reference)
    {
        s_ddt.CmdSetStencilReference(CommandBuffer, FaceMask, Reference);
    }

    void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipelineLayout Layout, uint32_t FirstSet, uint32_t DescriptorSetCount, const VkDescriptorSet* DescriptorSets, uint32_t DynamicOffsetCount, const uint32_t* DynamicOffsets)
    {
        s_ddt.CmdBindDescriptorSets(CommandBuffer, PipelineBindPoint, Layout, FirstSet, DescriptorSetCount, DescriptorSets, DynamicOffsetCount, DynamicOffsets);
    }

    void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, VkIndexType IndexType)
    {
        s_ddt.CmdBindIndexBuffer(CommandBuffer, Buffer, Offset, IndexType);
    }

    void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer CommandBuffer, uint32_t FirstBinding, uint32_t BindingCount, const VkBuffer* Buffers, const VkDeviceSize* Offsets)
    {
        s_ddt.CmdBindVertexBuffers(CommandBuffer, FirstBinding, BindingCount, Buffers, Offsets);
    }

    void VKAPI_CALL vkCmdDraw(VkCommandBuffer CommandBuffer, uint32_t VertexCount, uint32_t InstanceCount, uint32_t FirstVertex, uint32_t FirstInstance)
    {
        s_ddt.CmdDraw(CommandBuffer, VertexCount, InstanceCount, FirstVertex, FirstInstance);
    }

    void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer CommandBuffer, uint32_t IndexCount, uint32_t InstanceCount, uint32_t FirstIndex, int32_t VertexOffset, uint32_t FirstInstance)
    {
        s_ddt.CmdDrawIndexed(CommandBuffer, IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance);
    }

    void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, uint32_t DrawCount, uint32_t Stride)
    {
        s_ddt.CmdDrawIndirect(CommandBuffer, Buffer, Offset, DrawCount, Stride);
    }

    void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, uint32_t DrawCount, uint32_t Stride)
    {
        s_ddt.CmdDrawIndexedIndirect(CommandBuffer, Buffer, Offset, DrawCount, Stride);
    }

    void VKAPI_CALL vkCmdDispatch(VkCommandBuffer CommandBuffer, uint32_t X, uint32_t Y, uint32_t Z)
    {
        s_ddt.CmdDispatch(CommandBuffer, X, Y, Z);
    }

    void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset)
    {
        s_ddt.CmdDispatchIndirect(CommandBuffer, Buffer, Offset);
    }

    void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer CommandBuffer, VkBuffer SrcBuffer, VkBuffer DstBuffer, uint32_t RegionCount, const VkBufferCopy* Regions)
    {
        s_ddt.CmdCopyBuffer(CommandBuffer, SrcBuffer, DstBuffer, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkImageCopy* Regions)
    {
        s_ddt.CmdCopyImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkImageBlit* Regions, VkFilter Filter)
    {
        s_ddt.CmdBlitImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions, Filter);
    }

    void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer CommandBuffer, VkBuffer SrcBuffer, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkBufferImageCopy* Regions)
    {
        s_ddt.CmdCopyBufferToImage(CommandBuffer, SrcBuffer, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkBuffer DstBuffer, uint32_t RegionCount, const VkBufferImageCopy* Regions)
    {
        s_ddt.CmdCopyImageToBuffer(CommandBuffer, SrcImage, SrcImageLayout, DstBuffer, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer CommandBuffer, VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize DataSize, const void* pData)
    {
        s_ddt.CmdUpdateBuffer(CommandBuffer, DstBuffer, DstOffset, DataSize, pData);
    }

    void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer CommandBuffer, VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize Size, uint32_t Data)
    {
        s_ddt.CmdFillBuffer(CommandBuffer, DstBuffer, DstOffset, Size, Data);
    }

    void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer CommandBuffer, VkImage Image, VkImageLayout ImageLayout, const VkClearColorValue* Color, uint32_t RangeCount, const VkImageSubresourceRange* Ranges)
    {
        s_ddt.CmdClearColorImage(CommandBuffer, Image, ImageLayout, Color, RangeCount, Ranges);
    }

    void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer CommandBuffer, VkImage Image, VkImageLayout ImageLayout, const VkClearDepthStencilValue* DepthStencil, uint32_t RangeCount, const VkImageSubresourceRange* Ranges)
    {
        s_ddt.CmdClearDepthStencilImage(CommandBuffer, Image, ImageLayout, DepthStencil, RangeCount, Ranges);
    }

    void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer CommandBuffer, uint32_t AttachmentCount, const VkClearAttachment* Attachments, uint32_t RectCount, const VkClearRect* Rects)
    {
        s_ddt.CmdClearAttachments(CommandBuffer, AttachmentCount, Attachments, RectCount, Rects);
    }

    void VKAPI_CALL vkCmdResolveImage(
        VkCommandBuffer CommandBuffer,
        VkImage SrcImage, VkImageLayout SrcImageLayout,
        VkImage DstImage, VkImageLayout DstImageLayout,
        uint32_t RegionCount, const VkImageResolve* Regions)
    {
        s_ddt.CmdResolveImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer CommandBuffer, VkEvent Event, VkPipelineStageFlags StageMask)
    {
        s_ddt.CmdSetEvent(CommandBuffer, Event, StageMask);
    }

    void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer CommandBuffer, VkEvent Event, VkPipelineStageFlags StageMask)
    {
        s_ddt.CmdResetEvent(CommandBuffer, Event, StageMask);
    }

    void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer CommandBuffer, uint32_t EventCount, const VkEvent* Events,
        VkPipelineStageFlags SrcStageMask, VkPipelineStageFlags DstStageMask,
        uint32_t MemoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
        uint32_t BufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t ImageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers)
    {
        s_ddt.CmdWaitEvents(CommandBuffer, EventCount, Events, SrcStageMask, DstStageMask, MemoryBarrierCount, pMemoryBarriers,
            BufferMemoryBarrierCount, pBufferMemoryBarriers, ImageMemoryBarrierCount, pImageMemoryBarriers);
    }

    void VKAPI_CALL vkCmdPipelineBarrier(
        VkCommandBuffer CommandBuffer, VkPipelineStageFlags SrcStageMask, VkPipelineStageFlags DstStageMask, VkDependencyFlags DependencyFlags,
        uint32_t MemoryBarrierCount, const VkMemoryBarrier* MemoryBarriers,
        uint32_t BufferMemoryBarrierCount, const VkBufferMemoryBarrier* BufferMemoryBarriers,
        uint32_t ImageMemoryBarrierCount, const VkImageMemoryBarrier* ImageMemoryBarriers)
    {
        s_ddt.CmdPipelineBarrier(CommandBuffer, SrcStageMask, DstStageMask, DependencyFlags, MemoryBarrierCount, MemoryBarriers, BufferMemoryBarrierCount, BufferMemoryBarriers, ImageMemoryBarrierCount, ImageMemoryBarriers);
    }

    void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t Query, VkQueryControlFlags Flags)
    {
        s_ddt.CmdBeginQuery(CommandBuffer, QueryPool, Query, Flags);
    }

    void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t Query)
    {
        s_ddt.CmdEndQuery(CommandBuffer, QueryPool, Query);
    }

    void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t FirstQuery, uint32_t QueryCount)
    {
        s_ddt.CmdResetQueryPool(CommandBuffer, QueryPool, FirstQuery, QueryCount);
    }

    void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer CommandBuffer, VkPipelineStageFlagBits PipelineStage, VkQueryPool QueryPool, uint32_t Query)
    {
        s_ddt.CmdWriteTimestamp(CommandBuffer, PipelineStage, QueryPool, Query);
    }

    void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t FirstQuery, uint32_t QueryCount,
        VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize Stride, VkQueryResultFlags Flags)
    {
        s_ddt.CmdCopyQueryPoolResults(CommandBuffer, QueryPool, FirstQuery, QueryCount, DstBuffer, DstOffset, Stride, Flags);
    }

    void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer CommandBuffer, VkPipelineLayout Layout, VkShaderStageFlags StageFlags, uint32_t Offset, uint32_t Size, const void* pValues)
    {
        s_ddt.CmdPushConstants(CommandBuffer, Layout, StageFlags, Offset, Size, pValues);
    }

    void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer CommandBuffer, const VkRenderPassBeginInfo* RenderPassBegin, VkSubpassContents Contents)
    {
        s_ddt.CmdBeginRenderPass(CommandBuffer, RenderPassBegin, Contents);
    }

    void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer CommandBuffer, VkSubpassContents Contents)
    {
        s_ddt.CmdNextSubpass(CommandBuffer, Contents);
    }

    void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer CommandBuffer)
    {
        s_ddt.CmdEndRenderPass(CommandBuffer);
    }

    void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer CommandBuffer, uint32_t CommandBufferCount, const VkCommandBuffer* pCommandBuffers)
    {
        s_ddt.CmdExecuteCommands(CommandBuffer, CommandBufferCount, pCommandBuffers);
    }

    // -- Vulkan 1.1 ---

    VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* pApiVersion)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkEnumerateInstanceVersion)GetProcAddress(lib, "vkEnumerateInstanceVersion");
        return trampoline(pApiVersion);
    }

    VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos)
    {
        return s_ddt.BindBufferMemory2(device, bindInfoCount, pBindInfos);
    }

    VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos)
    {
        return s_ddt.BindImageMemory2(device, bindInfoCount, pBindInfos);
    }

    void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeatures(VkDevice device, uint32_t heapIndex, uint32_t localDeviceIndex, uint32_t remoteDeviceIndex, VkPeerMemoryFeatureFlags* pPeerMemoryFeatures)
    {
        s_ddt.GetDeviceGroupPeerMemoryFeatures(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
    }

    void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
    {
        s_ddt.CmdSetDeviceMask(commandBuffer, deviceMask);
    }

    void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        s_ddt.CmdDispatchBase(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
    }

    VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties)
    {
        return s_idt.EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    }

    void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements)
    {
        s_ddt.GetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
    }

    void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements)
    {
        s_ddt.GetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
    }

    void VKAPI_CALL vkGetImageSparseMemoryRequirements2(VkDevice device, const VkImageSparseMemoryRequirementsInfo2* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements)
    {
        s_ddt.GetImageSparseMemoryRequirements2(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
    }

    void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures)
    {
        s_idt.GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    }

    void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties)
    {
        s_idt.GetPhysicalDeviceProperties2(physicalDevice, pProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties)
    {
        s_idt.GetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties)
    {
        return s_idt.GetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties)
    {
        s_idt.GetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
    {
        s_idt.GetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo, uint32_t* pPropertyCount, VkSparseImageFormatProperties2* pProperties)
    {
        s_idt.GetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
    }

    void VKAPI_CALL vkTrimCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags)
    {
        s_ddt.TrimCommandPool(device, commandPool, flags);
    }

    void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue)
    {
        s_ddt.GetDeviceQueue2(device, pQueueInfo, pQueue);
    }

    VkResult VKAPI_CALL vkCreateSamplerYcbcrConversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSamplerYcbcrConversion* pYcbcrConversion)
    {
        return s_ddt.CreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
    }

    void VKAPI_CALL vkDestroySamplerYcbcrConversion(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const VkAllocationCallbacks* pAllocator)
    {
        s_ddt.DestroySamplerYcbcrConversion(device, ycbcrConversion, pAllocator);
    }

    VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate)
    {
        return s_ddt.CreateDescriptorUpdateTemplate(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    }

    void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator)
    {
        s_ddt.DestroyDescriptorUpdateTemplate(device, descriptorUpdateTemplate, pAllocator);
    }

    void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData)
    {
        s_ddt.UpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate, pData);
    }

    void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo, VkExternalBufferProperties* pExternalBufferProperties)
    {
        s_idt.GetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo, VkExternalFenceProperties* pExternalFenceProperties)
    {
        s_idt.GetPhysicalDeviceExternalFenceProperties(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo, VkExternalSemaphoreProperties* pExternalSemaphoreProperties)
    {
        s_idt.GetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
    }

    void VKAPI_CALL vkGetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, VkDescriptorSetLayoutSupport* pSupport)
    {
        s_ddt.GetDescriptorSetLayoutSupport(device, pCreateInfo, pSupport);
    }

    // -- Vulkan 1.2 ---

    void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        s_ddt.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        s_ddt.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return s_ddt.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    }

    void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin, const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        s_ddt.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo* pSubpassBeginInfo, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        s_ddt.CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    }

    void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        s_ddt.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    }

    void VKAPI_CALL vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
    {
        s_ddt.ResetQueryPool(device, queryPool, firstQuery, queryCount);
    }

    VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t* pValue)
    {
        return s_ddt.GetSemaphoreCounterValue(device, semaphore, pValue);
    }

    VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout)
    {
        return s_ddt.WaitSemaphores(device, pWaitInfo, timeout);
    }

    VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo)
    {
        return s_ddt.SignalSemaphore(device, pSignalInfo);
    }

    VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo)
    {
        return s_ddt.GetBufferDeviceAddress(device, pInfo);
    }

    uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo)
    {
        return s_ddt.GetBufferOpaqueCaptureAddress(device, pInfo);
    }

    uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo)
    {
        return s_ddt.GetDeviceMemoryOpaqueCaptureAddress(device, pInfo);
    }

    // -- Vulkan 1.3 --

    VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physicalDevice, uint32_t* pToolCount, VkPhysicalDeviceToolProperties* pToolProperties)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkGetPhysicalDeviceToolProperties)GetProcAddress(lib, "vkGetPhysicalDeviceToolProperties");
        return trampoline(physicalDevice, pToolCount, pToolProperties);
    }

    VkResult VKAPI_CALL vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPrivateDataSlot* pPrivateDataSlot)
    {
        return s_ddt.CreatePrivateDataSlot(device, pCreateInfo, pAllocator, pPrivateDataSlot);
    }

    void VKAPI_CALL vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks* pAllocator)
    {
        s_ddt.DestroyPrivateDataSlot(device, privateDataSlot, pAllocator);
    }

    VkResult VKAPI_CALL vkSetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data)
    {
        return s_ddt.SetPrivateData(device, objectType, objectHandle, privateDataSlot, data);
    }

    void VKAPI_CALL vkGetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t* pData)
    {
        s_ddt.GetPrivateData(device, objectType, objectHandle, privateDataSlot, pData);
    }

    void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* pDependencyInfo)
    {
        s_ddt.CmdSetEvent2(commandBuffer, event, pDependencyInfo);
    }

    void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask)
    {
        s_ddt.CmdResetEvent2(commandBuffer, event, stageMask);
    }

    void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, const VkDependencyInfo* pDependencyInfos)
    {
        s_ddt.CmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos);
    }

    void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo)
    {
        s_ddt.CmdPipelineBarrier2(commandBuffer, pDependencyInfo);
    }

    void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query)
    {
        s_ddt.CmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
    }

    VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence)
    {
        return s_ddt.QueueSubmit2(queue, submitCount, pSubmits, fence);
    }

    void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo)
    {
        s_ddt.CmdCopyBuffer2(commandBuffer, pCopyBufferInfo);
    }

    void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo)
    {
        s_ddt.CmdCopyImage2(commandBuffer, pCopyImageInfo);
    }

    void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo)
    {
        s_ddt.CmdCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo);
    }

    void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo)
    {
        s_ddt.CmdCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo);
    }

    void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo)
    {
        s_ddt.CmdBlitImage2(commandBuffer, pBlitImageInfo);
    }

    void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo)
    {
        s_ddt.CmdResolveImage2(commandBuffer, pResolveImageInfo);
    }

    void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        s_ddt.CmdBeginRendering(commandBuffer, pRenderingInfo);
    }

    void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
    {
        s_ddt.CmdEndRendering(commandBuffer);
    }

    void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
    {
        s_ddt.CmdSetCullMode(commandBuffer, cullMode);
    }

    void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
    {
        s_ddt.CmdSetFrontFace(commandBuffer, frontFace);
    }

    void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology)
    {
        s_ddt.CmdSetPrimitiveTopology(commandBuffer, primitiveTopology);
    }

    void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport* pViewports)
    {
        s_ddt.CmdSetViewportWithCount(commandBuffer, viewportCount, pViewports);
    }

    void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D* pScissors)
    {
        s_ddt.CmdSetScissorWithCount(commandBuffer, scissorCount, pScissors);
    }

    void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides)
    {
        s_ddt.CmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
    }

    void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
    {
        s_ddt.CmdSetDepthTestEnable(commandBuffer, depthTestEnable);
    }

    void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
    {
        s_ddt.CmdSetDepthWriteEnable(commandBuffer, depthWriteEnable);
    }

    void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
    {
        s_ddt.CmdSetDepthCompareOp(commandBuffer, depthCompareOp);
    }

    void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable)
    {
        s_ddt.CmdSetDepthBoundsTestEnable(commandBuffer, depthBoundsTestEnable);
    }

    void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
    {
        s_ddt.CmdSetStencilTestEnable(commandBuffer, stencilTestEnable);
    }

    void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp)
    {
        s_ddt.CmdSetStencilOp(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
    }

    void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
    {
        s_ddt.CmdSetRasterizerDiscardEnable(commandBuffer, rasterizerDiscardEnable);
    }

    void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
    {
        s_ddt.CmdSetDepthBiasEnable(commandBuffer, depthBiasEnable);
    }

    void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable)
    {
        s_ddt.CmdSetPrimitiveRestartEnable(commandBuffer, primitiveRestartEnable);
    }

    void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements)
    {
        s_ddt.GetDeviceBufferMemoryRequirements(device, pInfo, pMemoryRequirements);
    }

    void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements)
    {
        s_ddt.GetDeviceImageMemoryRequirements(device, pInfo, pMemoryRequirements);
    }

    void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements)
    {
        s_ddt.GetDeviceImageSparseMemoryRequirements(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
    }

    // -- VK_KHR_swapchain --

    VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice Device, const VkSwapchainCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSwapchainKHR* Swapchain)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_CreateSwapchainKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkCreateSwapchainKHRBefore*)hook)(Device, CreateInfo, Allocator, Swapchain, skip);
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_ddt.CreateSwapchainKHR(Device, CreateInfo, Allocator, Swapchain);
        }

        {
            const auto& hooks = sl::plugin_manager::getInterface()->getAfterHooks(sl::FunctionHookID::eVulkan_CreateSwapchainKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkCreateSwapchainKHRAfter*)hook)(Device, CreateInfo, Allocator, Swapchain);
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }
        return result;
    }

    void VKAPI_CALL vkDestroySwapchainKHR(VkDevice Device, VkSwapchainKHR Swapchain, const VkAllocationCallbacks* Allocator)
    {
        bool skip = false;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_DestroySwapchainKHR);
            for (auto [hook, feature] : hooks)
            {
                ((sl::PFunVkDestroySwapchainKHRBefore*)hook)(Device, Swapchain, Allocator, skip);
            }
        }

        if (!skip)
        {
            s_ddt.DestroySwapchainKHR(Device, Swapchain, Allocator);
        }
    }

    VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice Device, VkSwapchainKHR Swapchain, uint32_t* SwapchainImageCount, VkImage* SwapchainImages)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_GetSwapchainImagesKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkGetSwapchainImagesKHRBefore*)hook)(Device, Swapchain, SwapchainImageCount, SwapchainImages, skip);
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_ddt.GetSwapchainImagesKHR(Device, Swapchain, SwapchainImageCount, SwapchainImages);
        }
        return result;
    }

    VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice Device, VkSwapchainKHR Swapchain, uint64_t Timeout, VkSemaphore Semaphore, VkFence Fence, uint32_t* ImageIndex)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_AcquireNextImageKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkAcquireNextImageKHRBefore*)hook)(Device, Swapchain, Timeout, Semaphore, Fence, ImageIndex, skip);
                // report error on first fail
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_ddt.AcquireNextImageKHR(Device, Swapchain, Timeout, Semaphore, Fence, ImageIndex);
        }
        return result;
    }

    VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue Queue, const VkPresentInfoKHR* PresentInfo)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        auto hooksId = sl::FunctionHookID::eVulkan_Present;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(hooksId);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkQueuePresentKHRBefore*)hook)(Queue, PresentInfo, skip);
                // report error on first fail
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_ddt.QueuePresentKHR(Queue, PresentInfo);
        }

        {
            const auto& hooks = sl::plugin_manager::getInterface()->getAfterHooks(hooksId);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkQueuePresentKHRAfter*)hook)();
                // report error on first fail
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        return result;
    }

    // ppp: patch: begin
    VkResult VKAPI_CALL vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities)
    {
        return s_ddt.GetDeviceGroupPresentCapabilitiesKHR(device, pDeviceGroupPresentCapabilities);
    }

    VkResult VKAPI_CALL vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR* pModes)
    {
        return s_ddt.GetDeviceGroupSurfacePresentModesKHR(device, surface, pModes);
    }

    VkResult VKAPI_CALL vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pRectCount, VkRect2D* pRects)
    {
        return s_idt.GetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, pRectCount, pRects);
    }

    // not sure this works correctly if the plugin hook is not using the deviceMask in pAcquireInfo. Streamline needs to support this.
    VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_AcquireNextImageKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkAcquireNextImageKHRBefore*)hook)(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex, skip);
                // report error on first fail
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_ddt.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
        }
        return result;
    }
    // ppp: patch: end

    // ppp: patch: begin
    VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkDisplayPropertiesKHR* pProperties)
    {
        return s_idt.GetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, pPropertyCount, pProperties);
    }
    VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t * pPropertyCount, VkDisplayPlanePropertiesKHR * pProperties)
    {
        return s_idt.GetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, pPropertyCount, pProperties);
    }
    VkResult VKAPI_CALL vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice, uint32_t planeIndex, uint32_t * pDisplayCount, VkDisplayKHR * pDisplays)
    {
        return s_idt.GetDisplayPlaneSupportedDisplaysKHR(physicalDevice, planeIndex, pDisplayCount, pDisplays);
    }
    VkResult VKAPI_CALL vkGetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t * pPropertyCount, VkDisplayModePropertiesKHR * pProperties)
    {
        return s_idt.GetDisplayModePropertiesKHR(physicalDevice, display, pPropertyCount, pProperties);
    }
    VkResult VKAPI_CALL vkCreateDisplayModeKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, const VkDisplayModeCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDisplayModeKHR * pMode)
    {
        return s_idt.CreateDisplayModeKHR(physicalDevice, display, pCreateInfo, pAllocator, pMode);
    }
    VkResult VKAPI_CALL vkGetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkDisplayModeKHR mode, uint32_t planeIndex, VkDisplayPlaneCapabilitiesKHR * pCapabilities)
    {
        return s_idt.GetDisplayPlaneCapabilitiesKHR(physicalDevice, mode, planeIndex, pCapabilities);
    }
    VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR(VkInstance instance, const VkDisplaySurfaceCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkSurfaceKHR * pSurface)
    {
        return s_idt.CreateDisplayPlaneSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    // ppp: patch: end


    // -- VK_KHR_win32_surface

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, VkSurfaceCapabilitiesKHR* SurfaceCapabilities)
    {
        return s_idt.GetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, SurfaceCapabilities);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* SurfaceFormatCountPtr, VkSurfaceFormatKHR* SurfaceFormats)
    {
        return s_idt.GetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, SurfaceFormatCountPtr, SurfaceFormats);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice PhysicalDevice, uint32_t QueueFamilyIndex, VkSurfaceKHR Surface, VkBool32* SupportedPtr)
    {
        return s_idt.GetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, QueueFamilyIndex, Surface, SupportedPtr);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* PresentModeCountPtr, VkPresentModeKHR* PresentModesPtr)
    {
        return s_idt.GetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, PresentModeCountPtr, PresentModesPtr);
    }

    VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance Instance, const VkWin32SurfaceCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSurfaceKHR* Surface)
    {
        bool skip = false;
        VkResult result = VK_SUCCESS;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_CreateWin32SurfaceKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkCreateWin32SurfaceKHRBefore*)hook)(Instance, CreateInfo, Allocator, Surface, skip);
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
        }

        if (!skip)
        {
            result = s_idt.CreateWin32SurfaceKHR(Instance, CreateInfo, Allocator, Surface);
        }

        {
            const auto& hooks = sl::plugin_manager::getInterface()->getAfterHooks(sl::FunctionHookID::eVulkan_CreateWin32SurfaceKHR);
            for (auto [hook, feature] : hooks)
            {
                result = ((sl::PFunVkCreateWin32SurfaceKHRAfter*)hook)(Instance, CreateInfo, Allocator, Surface);
                if (result != VK_SUCCESS)
                {
                   return result;
                }
            }
        }
        return result;
    }

    void VKAPI_CALL vkDestroySurfaceKHR(VkInstance Instance, VkSurfaceKHR Surface, const VkAllocationCallbacks* pAllocator)
    {
        bool skip = false;
        {
            const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_DestroySurfaceKHR);
            for (auto [hook, feature] : hooks)
            {
                ((sl::PFunVkDestroySurfaceKHRBefore*)hook)(Instance, Surface, pAllocator, skip);
            }
        }

        if (!skip)
        {
            s_idt.DestroySurfaceKHR(Instance, Surface, pAllocator);
        }
    }

    // -- VK_KHR_get_physical_device_properties2

    void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures2KHR* Features)
    {
        s_idt.GetPhysicalDeviceFeatures2KHR(PhysicalDevice, Features);
    }

    void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties2KHR* Properties)
    {
        s_idt.GetPhysicalDeviceProperties2KHR(PhysicalDevice, Properties);
    }

    // -- VK_KHR_get_memory_requirements2 --

    void VKAPI_CALL vkGetImageMemoryRequirements2KHR(VkDevice Device, const VkImageMemoryRequirementsInfo2KHR* Info, VkMemoryRequirements2KHR* MemoryRequirements)
    {
        s_ddt.GetImageMemoryRequirements2KHR(Device, Info, MemoryRequirements);
    }

#define SL_INTERCEPT(F)          \
if (strcmp(pName, #F) == 0)          \
{                      \
  return (PFN_vkVoidFunction)F;      \
}

    PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return nullptr;
        }

        if (!s_ddt.GetDeviceProcAddr)
        {
            s_ddt.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
        }

        // Redirect only the hooks we need
        SL_INTERCEPT(vkGetInstanceProcAddr);
        SL_INTERCEPT(vkGetDeviceProcAddr);
        SL_INTERCEPT(vkQueuePresentKHR);
        SL_INTERCEPT(vkCreateImage);
        SL_INTERCEPT(vkCmdPipelineBarrier);
        SL_INTERCEPT(vkCmdBindPipeline);
        SL_INTERCEPT(vkCmdBindDescriptorSets);
        SL_INTERCEPT(vkCreateSwapchainKHR);
        SL_INTERCEPT(vkGetSwapchainImagesKHR);
        SL_INTERCEPT(vkDestroySwapchainKHR);
        SL_INTERCEPT(vkAcquireNextImageKHR);
        SL_INTERCEPT(vkAcquireNextImage2KHR);
        SL_INTERCEPT(vkBeginCommandBuffer);
        SL_INTERCEPT(vkDeviceWaitIdle);

        return s_ddt.GetDeviceProcAddr(device, pName);
    }

    PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return nullptr;
        }

        // this can be called before vkCreateInstance, so we may not have the pointer table set up yet
        if (!s_idt.GetInstanceProcAddr)
        {
            s_idt.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
        }
        
        // Redirect only the hooks we need
        SL_INTERCEPT(vkGetInstanceProcAddr);
        SL_INTERCEPT(vkGetDeviceProcAddr);
        SL_INTERCEPT(vkCreateInstance);
        SL_INTERCEPT(vkDestroyInstance);
        SL_INTERCEPT(vkCreateDevice);
        SL_INTERCEPT(vkDestroyDevice);
        SL_INTERCEPT(vkEnumeratePhysicalDevices);

        SL_INTERCEPT(vkQueuePresentKHR);
        SL_INTERCEPT(vkCreateImage);
        SL_INTERCEPT(vkCmdPipelineBarrier);
        SL_INTERCEPT(vkCmdBindPipeline);
        SL_INTERCEPT(vkCmdBindDescriptorSets);
        SL_INTERCEPT(vkCreateSwapchainKHR);
        SL_INTERCEPT(vkDestroySwapchainKHR);
        SL_INTERCEPT(vkGetSwapchainImagesKHR);
        SL_INTERCEPT(vkAcquireNextImageKHR);
        SL_INTERCEPT(vkAcquireNextImage2KHR);
        SL_INTERCEPT(vkBeginCommandBuffer);
        SL_INTERCEPT(vkDeviceWaitIdle);

        return s_idt.GetInstanceProcAddr(instance, pName);
    }

} // extern "C"
