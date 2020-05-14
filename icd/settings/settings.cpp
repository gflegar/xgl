/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  settings.cpp
 * @brief Contains implementation of Vulkan Settings Loader class.
 ***********************************************************************************************************************
 */

#include "include/vk_utils.h"
#include "settings/settings.h"

#include "palFile.h"
#include "palHashMapImpl.h"
#include "palAssert.h"
#include "palInlineFuncs.h"
#include "palSysMemory.h"

#include "devDriverServer.h"
#include "protocols/ddSettingsService.h"

using namespace DevDriver::SettingsURIService;

#include <sstream>
#include <climits>
#include <cmath>

using namespace Util;

namespace vk
{

// =====================================================================================================================
// Constructor for the SettingsLoader object.
VulkanSettingsLoader::VulkanSettingsLoader(
    Pal::IDevice*   pDevice,
    Pal::IPlatform* pPlatform,
    uint32_t        deviceId)
    :
    ISettingsLoader(pPlatform, static_cast<Pal::DriverSettings*>(&m_settings), g_vulkanNumSettings),
    m_pDevice(pDevice),
    m_pPlatform(pPlatform)
{
    Util::Snprintf(m_pComponentName, sizeof(m_pComponentName), "Vulkan%d", deviceId);
    memset(&m_settings, 0, sizeof(RuntimeSettings));
}

// =====================================================================================================================
VulkanSettingsLoader::~VulkanSettingsLoader()
{
    auto* pDevDriverServer = m_pPlatform->GetDevDriverServer();
    if (pDevDriverServer != nullptr)
    {
        auto* pSettingsService = pDevDriverServer->GetSettingsService();
        if (pSettingsService != nullptr)
        {
            pSettingsService->UnregisterComponent(m_pComponentName);
        }
    }
}

Result VulkanSettingsLoader::Init()
{
    Result ret = m_settingsInfoMap.Init();

    if (ret == Result::Success)
    {
        // Init Settings Info HashMap
        InitSettingsInfo();

        // Setup default values for the settings
        SetupDefaults();

        m_state = Pal::SettingsLoaderState::EarlyInit;
    }

    return ret;
}
// =====================================================================================================================
// Append sub path to root path to generate an absolute path.
static char* MakeAbsolutePath(
    char*       pDstPath,     ///< [in,out] destination path which is an absolute path.
    size_t      dstSize,      ///< [in]     Length of the destination path string.
    const char* pRootPath,    ///< [in]     Root path.
    const char* pSubPath)     ///< [in]     *Relative* path.
{
    VK_ASSERT((pDstPath != nullptr) && (pRootPath != nullptr) && (pSubPath != nullptr));

    // '/' works perfectly fine on Windows as file path separator character:
    std::ostringstream s;
    s << pRootPath << "/" << pSubPath;
    Strncpy(pDstPath, s.str().c_str(), dstSize);

    return pDstPath;
}

// =====================================================================================================================
// Override defaults based on system info. This *must* occurs after ReadSettings because it is used to add correct root path
void VulkanSettingsLoader::OverrideSettingsBySystemInfo()
{
    // Overrides all paths for debug files to expected values.
    // Now those directories in setting are all *relative*:
    // Relative to the path in the AMD_DEBUG_DIR environment variable, and if that env var isn't set, the location is
    // platform dependent. So we need to query the root path from device and then concatenate two strings (of the root
    // path and relative path of specific file) to final usable absolute path
    const char* pRootPath = m_pDevice->GetDebugFilePath();

    if (pRootPath != nullptr)
    {
        MakeAbsolutePath(m_settings.renderPassLogDirectory, sizeof(m_settings.renderPassLogDirectory),
                         pRootPath, m_settings.renderPassLogDirectory);
        MakeAbsolutePath(m_settings.pipelineDumpDir, sizeof(m_settings.pipelineDumpDir),
                         pRootPath, m_settings.pipelineDumpDir);
        MakeAbsolutePath(m_settings.shaderReplaceDir, sizeof(m_settings.shaderReplaceDir),
                         pRootPath, m_settings.shaderReplaceDir);

        MakeAbsolutePath(m_settings.pipelineProfileDumpFile, sizeof(m_settings.pipelineProfileDumpFile),
                         pRootPath, m_settings.pipelineProfileDumpFile);
#if ICD_RUNTIME_APP_PROFILE
        MakeAbsolutePath(m_settings.pipelineProfileRuntimeFile, sizeof(m_settings.pipelineProfileRuntimeFile),
                         pRootPath, m_settings.pipelineProfileRuntimeFile);
#endif
    }
}

// =====================================================================================================================
// Override defaults based on application profile.  This occurs before any CCC settings or private panel settings are
// applied.
VkResult VulkanSettingsLoader::OverrideProfiledSettings(
    const VkAllocationCallbacks* pAllocCb,
    uint32_t                     appVersion,
    AppProfile                   appProfile)
{
    VkResult result = VkResult::VK_SUCCESS;

    Pal::PalPublicSettings* pPalSettings = m_pDevice->GetPublicSettings();

    Pal::DeviceProperties* pInfo = static_cast<Pal::DeviceProperties*>(
                                        pAllocCb->pfnAllocation(pAllocCb->pUserData,
                                                                sizeof(Pal::DeviceProperties),
                                                                VK_DEFAULT_MEM_ALIGN,
                                                                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE));

    if (pInfo == nullptr)
    {
        result = VkResult::VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if (result == VkResult::VK_SUCCESS)
    {
        memset(pInfo, 0, sizeof(Pal::DeviceProperties));
        m_pDevice->GetProperties(pInfo);

        // In general, DCC is very beneficial for color attachments. If this is completely offset, maybe by increased
        // shader read latency or partial writes of DCC blocks, it should be debugged on a case by case basis.
        if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
        {
            m_settings.forceDccForColorAttachments = true;
        }

        if (appProfile == AppProfile::Doom)
        {
            m_settings.enableSpvPerfOptimal = true;

            m_settings.optColorTargetUsageDoesNotContainResolveLayout = true;

            // No gains were seen pre-GFX9
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp9)
            {
                m_settings.barrierFilterOptions = SkipStrayExecutionDependencies |
                    SkipImageLayoutUndefined |
                    SkipDuplicateResourceBarriers |
                    ForceImageSharingModeExclusive;
            }

            // Vega 20 has better performance on DOOM when DCC is disabled except for the 32 BPP surfaces
            if (pInfo->revision == Pal::AsicRevision::Vega20)
            {
                m_settings.dccBitsPerPixelThreshold = 32;
            }

            // id games are known to query instance-level functions with vkGetDeviceProcAddr illegally thus we
            // can't do any better than returning a non-null function pointer for them.
            m_settings.lenientInstanceFuncQuery = true;
        }

        if (appProfile == AppProfile::DoomVFR)
        {
            // id games are known to query instance-level functions with vkGetDeviceProcAddr illegally thus we
            // can't do any better than returning a non-null function pointer for them.
            m_settings.lenientInstanceFuncQuery = true;

            // This works around a crash at app startup.
            m_settings.ignoreSuboptimalSwapchainSize = true;

            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                if (pInfo->revision == Pal::AsicRevision::Navi14)
                {
                    m_settings.barrierFilterOptions = SkipImageLayoutUndefined;
                }
            }
        }

        if ((appProfile == AppProfile::WolfensteinII) ||
            (appProfile == AppProfile::WolfensteinYoungblood))
        {
            m_settings.enableSpvPerfOptimal = true;

            if (appProfile == AppProfile::WolfensteinII)
            {
                m_settings.zeroInitIlRegs = true;
            }

            m_settings.optColorTargetUsageDoesNotContainResolveLayout = true;

            // No gains were seen pre-GFX9
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp9)
            {
                m_settings.barrierFilterOptions = SkipStrayExecutionDependencies |
                    SkipImageLayoutUndefined |
                    ForceImageSharingModeExclusive;
            }

            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                m_settings.asyncComputeQueueLimit = 1;
            }

            // The Vega 20 PAL default is slower on Wolfenstein II, so always allow DCC.
            if (pInfo->revision == Pal::AsicRevision::Vega20)
            {
                m_settings.dccBitsPerPixelThreshold = 0;
            }

            // id games are known to query instance-level functions with vkGetDeviceProcAddr illegally thus we
            // can't do any better than returning a non-null function pointer for them.
            m_settings.lenientInstanceFuncQuery = true;
        }

        if (((appProfile == AppProfile::WolfensteinII) ||
            (appProfile == AppProfile::WolfensteinYoungblood) ||
            (appProfile == AppProfile::Doom)) &&
            (pInfo->gfxLevel == Pal::GfxIpLevel::GfxIp10_1))
        {
            m_settings.asyncComputeQueueMaxWavesPerCu = 40;
            m_settings.nggSubgroupSizing = NggSubgroupExplicit;
            m_settings.nggVertsPerSubgroup = 254;
            m_settings.nggPrimsPerSubgroup = 128;
        }

        if (appProfile == AppProfile::WorldWarZ)
        {
            m_settings.robustBufferAccess = FeatureForceEnable;

            m_settings.prefetchShaders = true;

            m_settings.optimizeCmdbufMode = EnableOptimizeCmdbuf;

            m_settings.usePalPipelineCaching = true;
            if (pInfo->revision == Pal::AsicRevision::Vega20)
            {
                m_settings.dccBitsPerPixelThreshold = 16;
            }

            // WWZ performs worse with DCC forced on, so just let the PAL heuristics decide what's best for now.
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                m_settings.forceDccForColorAttachments = false;
            }

        }

        if (appProfile == AppProfile::IdTechEngine)
        {
            m_settings.enableSpvPerfOptimal = true;

            // id games are known to query instance-level functions with vkGetDeviceProcAddr illegally thus we
            // can't do any better than returning a non-null function pointer for them.
            m_settings.lenientInstanceFuncQuery = true;
        }

        if (appProfile == AppProfile::Dota2)
        {
            pPalSettings->useGraphicsFastDepthStencilClear = true;

            //Vega 20 has better performance on Dota 2 when DCC is disabled.
            if (pInfo->revision == Pal::AsicRevision::Vega20)
            {
                m_settings.dccBitsPerPixelThreshold = 128;
            }
            m_settings.disableSmallSurfColorCompressionSize = 511;

            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.useAnisoThreshold = true;
            m_settings.anisoThreshold = 1.0f;

            m_settings.prefetchShaders = true;
            m_settings.disableMsaaStencilShaderRead = true;

            // Dota 2 will be the pilot for pal pipeline caching.
            m_settings.usePalPipelineCaching = true;
        }

        if (appProfile == AppProfile::Source2Engine)
        {
            pPalSettings->useGraphicsFastDepthStencilClear = true;

            m_settings.disableSmallSurfColorCompressionSize = 511;

            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.useAnisoThreshold = true;
            m_settings.anisoThreshold = 1.0f;

            m_settings.prefetchShaders = true;
            m_settings.disableMsaaStencilShaderRead = true;
        }

        if (appProfile == AppProfile::Talos)
        {
            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.optImgMaskToApplyShaderReadUsageForTransferSrc = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

            m_settings.forceDepthClampBasedOnZExport = true;
        }

        if (appProfile == AppProfile::SeriousSamFusion)
        {
            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.useAnisoThreshold = true;
            m_settings.anisoThreshold = 1.0f;

            m_settings.prefetchShaders = true;
        }

        if (appProfile == AppProfile::SedpEngine)
        {
            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
        }

        if (appProfile == AppProfile::StrangeBrigade)
        {

        }

        if (appProfile == AppProfile::MadMax)
        {
            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.useAnisoThreshold = true;
            m_settings.anisoThreshold = 1.0f;
        }

        if (appProfile == AppProfile::F1_2017)
        {
            m_settings.prefetchShaders = true;

            // F1 2017 performs worse with DCC forced on, so just let the PAL heuristics decide what's best for now.
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                m_settings.forceDccForColorAttachments = false;
            }
        }

        if (appProfile == AppProfile::ThronesOfBritannia)
        {
            m_settings.disableHtileBasedMsaaRead = true;
            m_settings.enableFullCopyDstOnly = true;
        }

        if (appProfile == AppProfile::DiRT4)
        {
            // DiRT 4 performs worse with DCC forced on, so just let the PAL heuristics decide what's best for now.
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                m_settings.forceDccForColorAttachments = false;
            }

            m_settings.forceDepthClampBasedOnZExport = true;
        }

        if (appProfile == AppProfile::WarHammerII)
        {
            // WarHammer II performs worse with DCC forced on, so just let the PAL heuristics decide
            // what's best for now.
            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                m_settings.forceDccForColorAttachments = false;
            }
        }

        if (appProfile == AppProfile::RainbowSixSiege)
        {
            m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
            m_settings.useAnisoThreshold = true;
            m_settings.anisoThreshold = 1.0f;

            // Ignore suboptimal swapchain size to fix crash on task switch
            m_settings.ignoreSuboptimalSwapchainSize = true;

            // Navi10 has better performance on Rainbow 6 Siege when dccBitsPerPixelThreshold is set to 64
            if (pInfo->revision == Pal::AsicRevision::Navi10)
            {
                m_settings.dccBitsPerPixelThreshold = 64;
            }
        }

        if (appProfile == AppProfile::Rage2 || appProfile == AppProfile::ApexEngine)
        {
            // Prefetching shaders gives us a 2.5% perf increase
            m_settings.prefetchShaders = true;

            //PM4 optimizations give us another 1.5% perf increase
            m_settings.optimizeCmdbufMode = OptimizeCmdbufMode::EnableOptimizeCmdbuf;

            // Rage 2 currently has all it's images set to VK_SHARING_MODE_CONCURRENT.
            // Forcing these images to use VK_SHARING_MODE_EXCLUSIVE gives us around 5% perf increase.
            m_settings.barrierFilterOptions = BarrierFilterOptions::ForceImageSharingModeExclusive;

            // Vega 20 seems to be do better on Rage 2 when dccBitsPerPixelThreshold is set to 16
            // 3-5% gain when exclusive sharing mode is enabled.
            if (pInfo->revision == Pal::AsicRevision::Vega20)
            {
                m_settings.dccBitsPerPixelThreshold = 16;
            }

            if (pInfo->gfxLevel >= Pal::GfxIpLevel::GfxIp10_1)
            {
                // Rage 2 performs worse with DCC forced on, so just let the PAL heuristics decide what's best for now.
                m_settings.forceDccForColorAttachments = false;
            }

        }

        if (appProfile == AppProfile::RedDeadRedemption2)
        {
            m_settings.enableAcquireBeforeSignal = true;

            m_settings.limitSampleCounts = VK_SAMPLE_COUNT_1_BIT |
                VK_SAMPLE_COUNT_2_BIT |
                VK_SAMPLE_COUNT_4_BIT;

            if (pInfo->gfxLevel == Pal::GfxIpLevel::GfxIp10_1)
            {
            }

            // Game does a lot of material swapping during the draws. Prefetching shaders gives us a gain of 2%.
            m_settings.prefetchShaders = true;

            // Force exclusive sharing mode - 2% gain
            m_settings.barrierFilterOptions = BarrierFilterOptions::ForceImageSharingModeExclusive;

            m_settings.delayFullScreenAcquireToFirstPresent = true;
        }

        if (appProfile == AppProfile::SaschaWillemsExamples)
        {
            m_settings.forceDepthClampBasedOnZExport = true;
        }

        // By allowing the enable/disable to be set by environment variable, any third party platform owners
        // can enable or disable the feature based on their internal feedback and not have to wait for a driver
        // update to catch issues

        const char* pPipelineCacheEnvVar = getenv(m_settings.pipelineCachingEnvironmentVariable);

        if (pPipelineCacheEnvVar != nullptr)
        {
            m_settings.usePalPipelineCaching = (atoi(pPipelineCacheEnvVar) >= 0);
        }

        pAllocCb->pfnFree(pAllocCb->pUserData, pInfo);
    }

    return result;
}

// =====================================================================================================================
// Writes the enumeration index of the chosen app profile to a file, whose path is determined via the VkPanel. Nothing
// will be written by default.
// TODO: Dump changes made due to app profile
void VulkanSettingsLoader::DumpAppProfileChanges(
    AppProfile         appProfile)
{
    if (m_settings.appProfileDumpDir[0] == '\0')
    {
        // Don't do anything if dump directory has not been set
        return;
    }

    wchar_t executableName[PATH_MAX];
    wchar_t executablePath[PATH_MAX];
    utils::GetExecutableNameAndPath(executableName, executablePath);

    char fileName[512] = {};
    Util::Snprintf(&fileName[0], sizeof(fileName), "%s/vkAppProfile.txt", &m_settings.appProfileDumpDir[0]);

    Util::File dumpFile;
    if (dumpFile.Open(fileName, Util::FileAccessAppend) == Pal::Result::Success)
    {
        dumpFile.Printf("Executable: %S%S\nApp Profile Enumeration: %d\n\n",
                        &executablePath[0],
                        &executableName[0],
                        static_cast<uint32_t>(appProfile));
        dumpFile.Close();
    }
}

// =====================================================================================================================
// Processes public and private panel settings for a particular PAL GPU.  Vulkan private settings and public CCC
// settings are first read and validated to produce the RuntimeSettings structure.  If PAL settings for the given GPU
// need to be updated based on the Vulkan settings, the PAL structure will also be updated.
VkResult VulkanSettingsLoader::ProcessSettings(
    const VkAllocationCallbacks* pAllocCb,
    uint32_t                     appVersion,
    AppProfile*                  pAppProfile)
{
    VkResult result = VkResult::VK_SUCCESS;

    const AppProfile origProfile = *pAppProfile;
    // Override defaults based on application profile
    result = OverrideProfiledSettings(pAllocCb, appVersion, *pAppProfile);

    if (result == VkResult::VK_SUCCESS)
    {
        // Read in the public settings from the Catalyst Control Center
        ReadPublicSettings();

        // Read the rest of the settings from the registry
        ReadSettings();

        // We need to override debug file paths settings to absolute paths as per system info
        OverrideSettingsBySystemInfo();

        DumpAppProfileChanges(*pAppProfile);

        if (m_settings.forceAppProfileEnable)
        {
            // Update application profile to the one from the panel
            *pAppProfile = static_cast<AppProfile>(m_settings.forceAppProfileValue);
        }

        // If we are changing profile via panel setting (i.e. forcing a specific profile), then
        // reload all settings.  This is because certain app profiles may override the default
        // values, and this allows the panel-mandated profile to override those defaults as well.
        if (*pAppProfile != origProfile)
        {
            result = ProcessSettings(pAllocCb, appVersion, pAppProfile);
        }
        else
        {
            // Register with the DevDriver settings service
            DevDriverRegister();
            m_state = Pal::SettingsLoaderState::LateInit;
        }
    }

    return result;
}

// =====================================================================================================================
// Reads the public settings set up by the Catalyst Control Center and sets the appropriate settings in the settings
// structure.
void VulkanSettingsLoader::ReadPublicSettings()
{
    // Read GPU ID (composed of PCI bus properties)
    uint32_t appGpuID = 0;
    if (m_pDevice->ReadSetting("AppGpuId",
        Pal::SettingScope::Global,
        Util::ValueType::Uint,
        &appGpuID,
        sizeof(appGpuID)))
    {
        m_settings.appGpuID = appGpuID;
    }

    // Read TurboSync global key
    bool turboSyncGlobal = false;
    if (m_pDevice->ReadSetting("TurboSync",
                                Pal::SettingScope::Global,
                                Util::ValueType::Boolean,
                                &turboSyncGlobal,
                                sizeof(turboSyncGlobal)))
    {
        m_settings.enableTurboSync = turboSyncGlobal;
    }

    // Read TFQ global key
    uint32_t texFilterQuality = TextureFilterOptimizationsEnabled;
    if (m_pDevice->ReadSetting("TFQ",
                                Pal::SettingScope::Global,
                                Util::ValueType::Uint,
                                &texFilterQuality,
                                sizeof(texFilterQuality)))
    {
        if (texFilterQuality <= TextureFilterOptimizationsAggressive)
        {
            m_settings.vulkanTexFilterQuality = static_cast<TextureFilterOptimizationSettings>(texFilterQuality);
        }
    }
}

// =====================================================================================================================
// Validates that the settings structure has legal values. Variables that require complicated initialization can also be
// initialized here.
void VulkanSettingsLoader::ValidateSettings()
{
    // Override the default preciseAnisoMode value based on the public CCC vulkanTexFilterQuality (TFQ) setting.
    // Note: This will override any Vulkan app specific profile.
    switch (m_settings.vulkanTexFilterQuality)
    {
    case TextureFilterOptimizationsDisabled:
        // Use precise aniso and disable optimizations.  Highest image quality.
        // This is acutally redundant because TFQ should cause the GPU's PERF_MOD field to be set in such a
        // way that all texture filtering optimizations are disabled anyway.
        m_settings.preciseAnisoMode = EnablePreciseAniso;
        break;

    case TextureFilterOptimizationsAggressive:
        // Enable both aniso and trilinear filtering optimizations. Lowest image quality.
        // This will cause Vulkan to fail conformance tests.
        m_settings.preciseAnisoMode = DisablePreciseAnisoAll;
        break;

    case TextureFilterOptimizationsEnabled:
        // This is the default.  Do nothing and maintain default settings.
        break;
    }

    // Disable FMASK MSAA reads if shadow desc VA range is not supported
    Pal::DeviceProperties deviceProps;
    m_pDevice->GetProperties(&deviceProps);

    if ((deviceProps.gpuMemoryProperties.flags.shadowDescVaSupport == 0) ||
        (deviceProps.gfxipProperties.srdSizes.fmaskView == 0))
    {
        m_settings.enableFmaskBasedMsaaRead = false;
    }

#if !VKI_GPUOPEN_PROTOCOL_ETW_CLIENT
    // Internal semaphore queue timing is always enabled when ETW is not available
    m_settings.devModeSemaphoreQueueTimingEnable = true;
#endif

}

// =====================================================================================================================
// Updates any PAL public settings based on our runtime settings if necessary.
void VulkanSettingsLoader::UpdatePalSettings()
{
    Pal::PalPublicSettings* pPalSettings = m_pDevice->GetPublicSettings();

    pPalSettings->textureOptLevel = m_settings.vulkanTexFilterQuality;

    pPalSettings->hintDisableSmallSurfColorCompressionSize = m_settings.disableSmallSurfColorCompressionSize;

    {
        Pal::DeviceProperties info;
        m_pDevice->GetProperties(&info);
        pPalSettings->useAcqRelInterface      = info.gfxipProperties.flags.supportReleaseAcquireInterface && m_settings.useAcqRelInterface;
        pPalSettings->enableGpuEventMultiSlot = m_settings.enableGpuEventMultiSlot;
    }

    // Setting disableSkipFceOptimization to false enables an optimization in PAL that disregards the FCE in a transition
    // if one of the built in clear colors are used (white/black) and the image is TCC compatible.
    pPalSettings->disableSkipFceOptimization = false;

#if VK_IS_PAL_VERSION_AT_LEAST(548, 1)
    // For vulkan driver, forceDepthClampBasedOnZExport should be false by default, this is required to pass depth_range_unrestricted CTS tests
    // Set it to true for applications that have perf drops
    pPalSettings->depthClampBasedOnZExport = m_settings.forceDepthClampBasedOnZExport;
#endif

}

// =====================================================================================================================
// The settings hashes are used during pipeline loading to verify that the pipeline data is compatible between when it
// was stored and when it was loaded.  The CCC controls some of the settings though, and the CCC doesn't set it
// identically across all GPUs in an MGPU configuration.  Since the CCC keys don't affect pipeline generation, just
// ignore those values when it comes to hash generation.
void VulkanSettingsLoader::GenerateSettingHash()
{
    // Temporarily ignore these CCC settings when computing a settings hash as described in the function header.
    uint32 appGpuID = m_settings.appGpuID;
    m_settings.appGpuID = 0;
    TextureFilterOptimizationSettings vulkanTexFilterQuality = m_settings.vulkanTexFilterQuality;
    m_settings.vulkanTexFilterQuality = TextureFilterOptimizationsDisabled;

    MetroHash128::Hash(
        reinterpret_cast<const uint8*>(&m_settings),
        sizeof(RuntimeSettings),
        m_settingHash.bytes);

    m_settings.appGpuID = appGpuID;
    m_settings.vulkanTexFilterQuality = vulkanTexFilterQuality;
}

// =====================================================================================================================
// Completes the initialization of the settings by overriding values from the registry and validating the final settings
// struct
void VulkanSettingsLoader::FinalizeSettings()
{
    m_state = Pal::SettingsLoaderState::Final;

    GenerateSettingHash();
}

};
