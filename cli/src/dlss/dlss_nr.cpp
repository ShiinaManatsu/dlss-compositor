#include "dlss_nr.h"
#include "ngx_wrapper.h"

#include "../core/log.h"

// NGX parameter interface + resource helpers. Feature 18 is NOT routed by the
// NGX core (NVSDK_NGX_VULKAN_CreateFeature(18) → 0xBAD0000B): like the Magpie
// fork we load nvngx_dlssnr.dll DIRECTLY and drive its exports (see
// docs/research/magpie-nr-notes.md). The parameter map itself stays core-owned
// (m_ngxParams) — the map ABI is the core↔snippet contract.
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_helpers_vk.h> // NVSDK_NGX_Create_ImageView_Resource_VK

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dlss {

namespace {

// NGX feature id 18 — DLSSNR / neural same-resolution filter (310.8.0 snippet).
constexpr NVSDK_NGX_Feature kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);

constexpr wchar_t kNrSnippetDll[] = L"nvngx_dlssnr.dll";

// Application id the signed snippet accepts (Magpie DLSSNR_SIGNED_SNIPPET_
// APPLICATION_ID — the NVIDIA sample id it uses for the same direct route).
constexpr unsigned long long kNrSnippetAppId = 0x0876232Cull;

// ── Snippet caller-compatibility shim ─────────────────────────────────────────
// The signed 310.8 snippet verifies its host during Init: it queries
// GetModuleFileNameW on the module of its caller and requires the name
// "nvngx.dll", failing with NVSDK_NGX_Result_FAIL_PlatformError (0xBAD00002)
// otherwise. Same workaround as the Magpie fork (SAOG0721/Magpie @ 8dfdf37,
// InstallSnippetCallerCompatibility): patch the snippet's IAT entry for
// GetModuleFileNameW so host-module queries report the authorized name.
std::atomic<HMODULE> g_nrHookCallerModule = nullptr;

using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
std::atomic<GetModuleFileNameWFn> g_nrOriginalGetModuleFileNameW = nullptr;
void** g_nrHookIatSlot = nullptr;

DWORD WINAPI NrGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    if (module && module == g_nrHookCallerModule.load(std::memory_order_acquire)) {
        constexpr wchar_t kAuthorizedCaller[] = L"nvngx.dll";
        constexpr DWORD kAuthorizedCallerLength =
            static_cast<DWORD>(std::size(kAuthorizedCaller)) - 1;
        if (!filename || size == 0) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= kAuthorizedCallerLength) {
            if (size > 1) {
                std::memcpy(filename, kAuthorizedCaller, (size - 1) * sizeof(wchar_t));
            }
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, kAuthorizedCaller, sizeof(kAuthorizedCaller));
        return kAuthorizedCallerLength;
    }

    const auto original =
        g_nrOriginalGetModuleFileNameW.load(std::memory_order_acquire);
    if (original) return original(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

/// Locate the IAT slot the snippet imports `functionName` through (x64 PE walk;
/// kernel32 and its api-ms libraryloader aliases, as in Magpie).
void** findImportedFunctionSlot(HMODULE mod, const char* functionName) {
    if (!mod || !functionName) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size ||
        directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
        directory.Size > nt->OptionalHeader.SizeOfImage) {
        return nullptr;
    }

    const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress + directory.Size);
    for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
             base + directory.VirtualAddress);
         descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
        if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
        const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + descriptor->OriginalFirstThunk);
        auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + descriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
            if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + nameRva);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            functionName) == 0) {
                return reinterpret_cast<void**>(&addressThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool installSnippetCallerHook(HMODULE snippetModule) {
    g_nrHookIatSlot = findImportedFunctionSlot(snippetModule, "GetModuleFileNameW");
    if (!g_nrHookIatSlot) {
        LOG_ERROR("[DLSS NR] snippet has no GetModuleFileNameW import — caller shim unavailable");
        return false;
    }

    HMODULE callerModule = nullptr;  // module containing the hook = host exe
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&NrGetModuleFileNameW),
                            &callerModule)) {
        LOG_ERROR("[DLSS NR] cannot resolve host module for caller shim (GetLastError={})",
                  GetLastError());
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_nrHookIatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        LOG_ERROR("[DLSS NR] cannot make snippet IAT writable (GetLastError={})",
                  GetLastError());
        return false;
    }

    g_nrHookCallerModule.store(callerModule, std::memory_order_release);
    void* original = InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(g_nrHookIatSlot),
        reinterpret_cast<void*>(&NrGetModuleFileNameW));
    GetModuleFileNameWFn originalFn = nullptr;
    static_assert(sizeof(originalFn) == sizeof(original));
    std::memcpy(&originalFn, &original, sizeof(originalFn));
    g_nrOriginalGetModuleFileNameW.store(originalFn, std::memory_order_release);

    DWORD ignoredProtect = 0;
    VirtualProtect(g_nrHookIatSlot, sizeof(void*), oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), g_nrHookIatSlot, sizeof(void*));

    if (!originalFn) {
        LOG_ERROR("[DLSS NR] snippet GetModuleFileNameW import resolved to null");
        return false;
    }
    LOG_INFO("[DLSS NR] snippet caller shim installed (host reports as nvngx.dll)");
    return true;
}

void restoreSnippetCallerHook() {
    if (!g_nrHookIatSlot) return;
    const auto original =
        g_nrOriginalGetModuleFileNameW.load(std::memory_order_acquire);
    if (original) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_nrHookIatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            void* target = nullptr;
            std::memcpy(&target, &original, sizeof(target));
            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(g_nrHookIatSlot), target);
            DWORD ignoredProtect = 0;
            VirtualProtect(g_nrHookIatSlot, sizeof(void*), oldProtect, &ignoredProtect);
            FlushInstructionCache(GetCurrentProcess(), g_nrHookIatSlot, sizeof(void*));
        }
    }
    g_nrOriginalGetModuleFileNameW.store(nullptr, std::memory_order_release);
    g_nrHookCallerModule.store(nullptr, std::memory_order_release);
    g_nrHookIatSlot = nullptr;
}

bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(extensions.begin(), extensions.end(),
        [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

NrSnippetTable loadNrSnippet() {
    wchar_t exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        throw std::runtime_error("DLSSNR: cannot resolve exe directory");
    }
    const std::wstring dllPath =
        (std::filesystem::path(exePath).parent_path() / kNrSnippetDll).wstring();

    // DLSS PG §5.1 — feature DLLs live next to the exe.
    // Search flags as Magpie: load-dir first, default system dirs for deps.
    HMODULE mod = LoadLibraryExW(dllPath.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        throw std::runtime_error(
            "DLSSNR: " + std::filesystem::path(dllPath).filename().string() +
            " not found next to the exe (GetLastError=" + std::to_string(GetLastError()) + ")");
    }

    NrSnippetTable s;
    s.library   = mod;
    s.init     = reinterpret_cast<decltype(s.init)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_Init_Ext2"));
    s.create   = reinterpret_cast<decltype(s.create)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_CreateFeature"));
    s.evaluate = reinterpret_cast<decltype(s.evaluate)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    s.release  = reinterpret_cast<decltype(s.release)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    s.shutdown = reinterpret_cast<decltype(s.shutdown)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_Shutdown1"));
    s.instanceExtReqs = reinterpret_cast<decltype(s.instanceExtReqs)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements"));
    s.deviceExtReqs = reinterpret_cast<decltype(s.deviceExtReqs)>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements"));

    if (!s.init || !s.create || !s.evaluate || !s.release || !s.shutdown) {
        FreeLibrary(mod);
        throw std::runtime_error("DLSSNR: nvngx_dlssnr.dll is missing Vulkan exports");
    }

    // Caller verification runs on every snippet entry point (including the
    // extension-requirement queries), so the shim must be live before Init.
    if (!installSnippetCallerHook(mod)) {
        FreeLibrary(mod);
        throw std::runtime_error(
            "DLSSNR: signed snippet caller-compatibility shim failed to install");
    }
    return s;
}

/// Log any instance/device extensions the snippet requires that our instance /
/// device do not enable. Returns false (and logs) when something is missing.
bool checkSnippetExtensions(const NrSnippetTable& s,
                            const wchar_t* appDataDir,
                            VkInstance instance, VkPhysicalDevice pd) {
    if (!s.instanceExtReqs || !s.deviceExtReqs) return true;  // best effort

    NVSDK_NGX_FeatureDiscoveryInfo di{};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = kFeatureDlssNr;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
    di.Identifier.v.ApplicationId = kNrSnippetAppId;
    di.ApplicationDataPath = appDataDir;
    di.FeatureInfo = nullptr;

    bool ok = true;

    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_SUCCEED(s.instanceExtReqs(&di, &count, &props)) && props) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!hasInstanceExtension(props[i].extensionName)) {
                LOG_ERROR("[DLSS NR] missing instance extension: {}", props[i].extensionName);
                ok = false;
            }
        }
    }

    count = 0;
    props = nullptr;
    if (NVSDK_NGX_SUCCEED(s.deviceExtReqs(instance, pd, &di, &count, &props)) && props) {
        uint32_t availCount = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> avail(availCount);
        if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &availCount, avail.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                const bool has = std::any_of(avail.begin(), avail.end(),
                    [&](const VkExtensionProperties& e) {
                        return std::strcmp(e.extensionName, props[i].extensionName) == 0;
                    });
                if (!has) {
                    LOG_ERROR("[DLSS NR] missing device extension: {}", props[i].extensionName);
                    ok = false;
                }
            }
        }
    }
    return ok;
}

/// Build an NGX Vulkan resource descriptor for a 2-D color attachment.
NVSDK_NGX_Resource_VK makeNrResource(VkImageView view,
                                     VkImage     image,
                                     uint32_t    width,
                                     uint32_t    height,
                                     VkFormat    format,
                                     bool        readWrite) {
    VkImageSubresourceRange range{};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;
    // DLSS SR §3.4 — NGX VK resources need both the view and the backing image.
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        view, image, range, format, width, height, readWrite);
}

} // namespace

DlssNrPass::DlssNrPass(NgxContext* ngx, VulkanContext* ctx)
    : m_ngx(ngx), m_ctx(ctx) {
    if (!ngx) throw std::invalid_argument("DlssNrPass: NgxContext must not be null");
    if (!ctx) throw std::invalid_argument("DlssNrPass: VulkanContext must not be null");
    m_ngxParams = m_ngx->params();
}

DlssNrPass::~DlssNrPass() {
    destroy();
}

void DlssNrPass::create(uint32_t width, uint32_t height, const NrSettings& settings) {
    if (m_handle) return;  // already created
    if (width == 0 || height == 0) {
        throw std::runtime_error("DlssNrPass::create: invalid resolution");
    }

    m_width   = width;
    m_height  = height;
    m_settings = settings;

    // ── Load the snippet and init it on OUR Vulkan device ─────────────────────
    // The 310.8.0.0 snippet exports the full Vulkan API surface; it is its own
    // NGX module instance, independent of the driver NGX core session.
    NrSnippetTable snippet = loadNrSnippet();

    // DLSS SR §3.5 — Init with the exe dir as application data path (logs only).
    wchar_t exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring appDataDir = (n != 0 && n < MAX_PATH)
        ? std::filesystem::path(exePath).parent_path().wstring()
        : std::filesystem::current_path().wstring();

    // Best-effort diagnostics: 310.8 snippet may require more Vulkan extensions
    // than the 310.6 core session requested at device creation.
    checkSnippetExtensions(snippet, appDataDir.c_str(),
                           m_ctx->instance(), m_ctx->physicalDevice());

    const NVSDK_NGX_Result ir = snippet.init(
        kNrSnippetAppId,                  // Magpie signed-snippet application id
        appDataDir.c_str(),
        m_ctx->instance(),
        m_ctx->physicalDevice(),
        m_ctx->device(),
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr,
        NVSDK_NGX_Version_API,
        nullptr);

    if (NVSDK_NGX_FAILED(ir)) {
        restoreSnippetCallerHook();
        FreeLibrary(static_cast<HMODULE>(snippet.library));
        throw std::runtime_error(
            "DLSSNR snippet Init_Ext2 failed: NVSDK_NGX_Result=" + std::to_string(ir));
    }
    m_snippet = snippet;

    // ── Creation-time geometry (Magpie SetCreateParametersUnsafe equivalents)
    m_ngxParams->Set("DLSSNR.Width",  static_cast<int>(width));
    m_ngxParams->Set("DLSSNR.Height", static_cast<int>(height));
    m_ngxParams->Set("DLSSNR.InputWidth",  static_cast<int>(width));
    m_ngxParams->Set("DLSSNR.InputHeight", static_cast<int>(height));
    m_ngxParams->Set("DLSSNR.OutputWidth",  static_cast<int>(width));
    m_ngxParams->Set("DLSSNR.OutputHeight", static_cast<int>(height));
    m_ngxParams->Set("DLSSNR.Output.Width",  static_cast<int>(width));
    m_ngxParams->Set("DLSSNR.Output.Height", static_cast<int>(height));
    m_ngxParams->Set("DLSSNR.Upscaling", 0);           // denoise-only, same resolution
    m_ngxParams->Set("DLSSNR.Scale", 1.0f);
    m_ngxParams->Set("DLSSNR.ScalingRatio", 1.0f);
    m_ngxParams->Set("DLSSNR.Hint.Render.Preset", 1);  // DEFAULT_NR_PRESET (not user-facing)

    // Stock NGX keys the snippet also reads.
    m_ngxParams->Set(NVSDK_NGX_Parameter_Width,  static_cast<int>(width));
    m_ngxParams->Set(NVSDK_NGX_Parameter_Height, static_cast<int>(height));
    m_ngxParams->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                     static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
    m_ngxParams->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1);
    m_ngxParams->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);

    // Tuning params — re-applied on every evaluate so the NR tool can retune
    // live (Magpie sets them in SetEvaluateParametersUnsafe for the same reason).
    applyTuningParams();
    LOG_INFO("[DLSS NR] guidance mode={} (0=all 1=zero 2=motion 3=depth)",
             static_cast<int>(m_settings.guidanceMode));

    VkCommandPool cmdPool = m_ctx->createComputeCommandPool();
    VkCommandBuffer initCmd = m_ctx->beginOneTimeCommands(cmdPool);

    const NVSDK_NGX_Result r = m_snippet.create( // feature 18 — DLSSNR (310.8.0 snippet)
        initCmd, kFeatureDlssNr, m_ngxParams, &m_handle);

    m_ctx->endOneTimeCommands(cmdPool, initCmd, m_ctx->computeQueue());
    vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);

    if (NVSDK_NGX_FAILED(r) || !m_handle) {
        m_handle = nullptr;
        (void)m_snippet.shutdown(m_ctx->device());
        restoreSnippetCallerHook();
        FreeLibrary(static_cast<HMODULE>(m_snippet.library));
        m_snippet = {};
        throw std::runtime_error(
            "DLSSNR create failed (feature 18): NVSDK_NGX_Result=" + std::to_string(r));
    }

    LOG_INFO("[DLSS NR] Created feature 18 same-res {}x{} style={} intensity={:.2f} "
             "localTone={:.2f} localStructure={:.2f} autoMask={}",
             width, height, static_cast<int>(m_settings.style), m_settings.intensity,
             m_settings.localToneStrength, m_settings.localStructureStrength,
             m_settings.useAutoMask ? 1 : 0);
}

void DlssNrPass::updateSettings(const NrSettings& settings) {
    m_settings = settings;
    if (m_handle) applyTuningParams();
}

/// Per-evaluate tuning keys — mirrors Magpie's SetEvaluateParametersUnsafe
/// (style/intensity/tone/structure/autoMask are evaluate-time in Magpie too).
/// `DLSSNR.Enabled` gates the feature per-eval; UICorrection stays off (we are
/// not a HUD compositor).
void DlssNrPass::applyTuningParams() {
    m_ngxParams->Set("DLSSNR.Style",       static_cast<int>(m_settings.style));
    m_ngxParams->Set("DLSSNR.Intensity",   m_settings.intensity);
    m_ngxParams->Set("DLSSNR.LocalToneStrength",      m_settings.localToneStrength);
    m_ngxParams->Set("DLSSNR.LocalStructureStrength", m_settings.localStructureStrength);
    m_ngxParams->Set("DLSSNR.UseAutoMask", m_settings.useAutoMask ? 1 : 0);
    m_ngxParams->Set("DLSSNR.Enabled", 1);
    m_ngxParams->Set("DLSSNR.UICorrection", 0);
}

void DlssNrPass::evaluate(VkCommandBuffer cmd, const NrInputs& in) {
    if (!m_handle) {
        throw std::runtime_error("DlssNrPass::evaluate called before create()");
    }
    if (in.colorView == VK_NULL_HANDLE || in.outputView == VK_NULL_HANDLE) {
        throw std::runtime_error("DlssNrPass::evaluate: missing color/output view");
    }

    // Resources: whole-frame color/output; guides at render res (subrect).
    auto rColor  = makeNrResource(in.colorView,  in.colorImage,  m_width, m_height,
                                  VK_FORMAT_R8G8B8A8_UNORM, false);
    auto rOutput = makeNrResource(in.outputView, in.outputImage, m_width, m_height,
                                  VK_FORMAT_R8G8B8A8_UNORM, true);

    const bool useMotion = in.mvView != VK_NULL_HANDLE;
    const bool useDepth  = in.depthView != VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK rMotion{};
    NVSDK_NGX_Resource_VK rDepth{};
    if (useMotion) {
        rMotion = makeNrResource(in.mvView, in.mvImage, in.renderWidth, in.renderHeight,
                                 VK_FORMAT_R16G16_SFLOAT, false);
    }
    if (useDepth) {
        rDepth = makeNrResource(in.depthView, in.depthImage, in.renderWidth, in.renderHeight,
                                VK_FORMAT_R32_SFLOAT, false);
    }

    // Subrects: color/output full frame; guides carry the render-res extent so
    // the feature can consume lower-resolution guidance.
    const auto setSubrect = [&](const char* base, uint32_t w, uint32_t h) {
        const std::string prefix(base);
        m_ngxParams->Set((prefix + "SubrectBaseX").c_str(), 0);
        m_ngxParams->Set((prefix + "SubrectBaseY").c_str(), 0);
        m_ngxParams->Set((prefix + "SubrectWidth").c_str(),  static_cast<int>(w));
        m_ngxParams->Set((prefix + "SubrectHeight").c_str(), static_cast<int>(h));
    };
    setSubrect("DLSSNR.Color", m_width, m_height);
    setSubrect("DLSSNR.Output", m_width, m_height);
    if (useMotion) {
        setSubrect("DLSSNR.MVec", in.renderWidth, in.renderHeight);
        m_ngxParams->Set("DLSSNR.MVecScaleX", 1.0f);  // DLSS SR §3.6.3 — pixel-unit MVs
        m_ngxParams->Set("DLSSNR.MVecScaleY", 1.0f);
    }
    if (useDepth) {
        setSubrect("DLSSNR.Depth", in.renderWidth, in.renderHeight);
        m_ngxParams->Set("DLSSNR.DepthInverted", 1);  // NDC depth (1 = far)
    }

    // Resources are passed as opaque pointers on the parameter block.
    m_ngxParams->Set("DLSSNR.Color",  static_cast<void*>(&rColor));
    m_ngxParams->Set("DLSSNR.Output", static_cast<void*>(&rOutput));
    if (useMotion) m_ngxParams->Set("DLSSNR.MVec",  static_cast<void*>(&rMotion));
    if (useDepth)  m_ngxParams->Set("DLSSNR.Depth", static_cast<void*>(&rDepth));

    // Reset on both the stock key and the snippet's own key (Magpie uses
    // "DLSSNR.Reset"; the stock key covers other 310.x builds).
    m_ngxParams->Set("DLSSNR.Reset", in.reset ? 1 : 0);
    m_ngxParams->Set(NVSDK_NGX_Parameter_Reset, in.reset ? 1 : 0); // DLSS §3.13
    m_ngxParams->Set(NVSDK_NGX_Parameter_Width,  static_cast<int>(m_width));
    m_ngxParams->Set(NVSDK_NGX_Parameter_Height, static_cast<int>(m_height));

    const NVSDK_NGX_Result r = m_snippet.evaluate(cmd, m_handle, m_ngxParams, nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        throw std::runtime_error("DLSSNR evaluate failed: " + std::to_string(r));
    }
}

void DlssNrPass::destroy() {
    if (m_handle) {
        // DLSS SR §5.3 — Release the feature handle before shutdown.
        (void)m_snippet.release(m_handle); // DLSS §5.3
        m_handle = nullptr;
    }
    if (m_snippet.library) {
        (void)m_snippet.shutdown(m_ctx->device());
        restoreSnippetCallerHook();
        FreeLibrary(static_cast<HMODULE>(m_snippet.library));
        m_snippet = {};
    }
}

} // namespace dlss
