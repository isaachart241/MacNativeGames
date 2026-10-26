/* Thor T0 probe (THOR_SCOPE.md): what the Android host and the Vulkan backend can rely on, measured on the device.
   Build:  $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android33-clang -O2 -o thor_probe thor_probe.c -lvulkan
   Run:    adb push thor_probe /data/local/tmp/ && adb shell /data/local/tmp/thor_probe
   Part 1 checks the host assumptions (thread pointer registers, Bionic's app TLS slot, code written at run time, write
   faults, fixed reservations below 2^39); part 2 lists the Vulkan features, formats, limits and memory the backend needs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void section(const char *s) { printf("\n== %s\n", s); }
static void yesno(const char *what, int v) { printf("  %-46s %s\n", what, v ? "yes" : "NO"); }

/* ---------- part 1: host ---------- */
static uint64_t read_tpidr(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static uint64_t read_tpidrro(void) { uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v; }

static volatile void *fault_addr; static volatile int faults; static void *fault_page; static size_t pagesz;
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc; fault_addr = si->si_addr; faults++;
    mprotect(fault_page, pagesz, PROT_READ | PROT_WRITE);   /* the dirty tracker's move: unprotect and let the store retry */
}

static void host_checks(void) {
    section("host");
    pagesz = (size_t)sysconf(_SC_PAGESIZE);
    printf("  page size %zu\n", pagesz);
    uint64_t tp = read_tpidr(), tro = read_tpidrro();
    printf("  tpidr_el0 %#" PRIx64 "  tpidrro_el0 %#" PRIx64 "\n", tp, tro);
    yesno("tpidrro_el0 usable as a thread pointer", tro != 0);
    uint64_t *slots = (uint64_t *)tp;   /* Bionic: slots at the thread pointer, TLS_SLOT_APP = 2 on arm64 */
    printf("  bionic slots 0..7:");
    for (int i = 0; i < 8; i++) printf(" %#" PRIx64, slots[i]);
    printf("\n");
    uint64_t was = slots[2]; slots[2] = 0x1234abcd; int kept = slots[2] == 0x1234abcd; slots[2] = was;
    yesno("TLS_SLOT_APP (slot 2) empty and writable", was == 0 && kept);

    /* code written at run time: anonymous RW, copy, RX (how loader.c maps the rewritten modules) */
    uint32_t code[2] = { 0x52800540, 0xd65f03c0 };   /* mov w0, #42 ; ret */
    void *x = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ran = 0;
    if (x != MAP_FAILED) {
        memcpy(x, code, sizeof code); __builtin___clear_cache((char *)x, (char *)x + sizeof code);
        if (!mprotect(x, pagesz, PROT_READ | PROT_EXEC)) ran = ((int (*)(void))x)() == 42;
        else printf("  mprotect RX: %s\n", strerror(errno));
    }
    yesno("anonymous memory made executable runs", ran);

    /* write fault on a protected page, handler unprotects, store retries (nvn_dirty.c) */
    struct sigaction sa = {0}, old; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, &old);
    fault_page = mmap(NULL, pagesz, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ((volatile uint32_t *)fault_page)[3] = 7;
    yesno("write fault caught, page unprotected, store retried", faults == 1 && fault_addr == (char *)fault_page + 12 && ((volatile uint32_t *)fault_page)[3] == 7);
    mprotect(fault_page, pagesz, PROT_READ);
    int fd = open("/proc/self/stat", O_RDONLY); ssize_t r = fd >= 0 ? pread(fd, fault_page, 16, 0) : -2; int err = errno; if (fd >= 0) close(fd);
    printf("  pread into a write-protected page: %zd (%s)\n", r, r < 0 ? strerror(err) : "no error");
    sigaction(SIGSEGV, &old, NULL);

    /* free address space below 2^39 and a large fixed reservation (services.c memory_init) */
    FILE *m = fopen("/proc/self/maps", "r"); char line[512]; uint64_t prev = 1ull << 32, best = 0, best_at = 0, top = 0;
    while (m && fgets(line, sizeof line, m)) {
        uint64_t a, b; if (sscanf(line, "%" SCNx64 "-%" SCNx64, &a, &b) != 2) continue;
        if (b > top) top = b;
        if (a > prev && a - prev > best) { best = a - prev; best_at = prev; }
        if (b > prev) prev = b;
    }
    if (m) fclose(m);
    printf("  highest mapping ends at %#" PRIx64 " (%s), largest gap above 4 GiB: %#" PRIx64 " bytes at %#" PRIx64 "\n",
           top, top <= (1ull << 39) ? "39-bit" : top <= (1ull << 48) ? "48-bit" : "?", best, best_at);
    uint64_t want = 64ull << 30, at = (best_at + (1ull << 30) - 1) & ~((1ull << 30) - 1);
    void *res = mmap((void *)at, want, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    printf("  64 GiB PROT_NONE reservation at %#" PRIx64 ": %s\n", at, res == (void *)at ? "ok" : strerror(errno));
    if (res == (void *)at) {   /* commit 1 GiB of it the way a heap grows */
        void *c = mmap((void *)at, 1ull << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        yesno("1 GiB committed inside the reservation", c == (void *)at);
        munmap(res, want);
    }
    int mfd = (int)syscall(SYS_memfd_create, "probe", 0);
    yesno("memfd_create (page aliasing)", mfd >= 0);
    if (mfd >= 0) {
        int ok = ftruncate(mfd, (off_t)pagesz) == 0;
        uint8_t *a = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0), *b = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (ok && a != MAP_FAILED && b != MAP_FAILED) { a[5] = 99; ok = b[5] == 99; } else ok = 0;
        yesno("two views of one memfd see the same bytes", ok);
        close(mfd);
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); printf("  cpus online %ld\n", cpus);
}

/* ---------- part 2: Vulkan ---------- */
static VkExtensionProperties *exts; static uint32_t next;
static int has_ext(const char *n) { for (uint32_t i = 0; i < next; i++) if (!strcmp(exts[i].extensionName, n)) return 1; return 0; }
static const char *fmt_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1"; case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3"; case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5"; case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H unsigned"; case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H signed";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7"; case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7 sRGB"; case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return "ASTC 4x4";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10 float"; case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16 float";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32 float"; case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8"; case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8 sRGB"; case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return "RGB9E5";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32F S8"; case VK_FORMAT_D24_UNORM_S8_UINT: return "D24 S8"; case VK_FORMAT_D32_SFLOAT: return "D32F";
    case VK_FORMAT_D16_UNORM: return "D16"; case VK_FORMAT_S8_UINT: return "S8"; case VK_FORMAT_R8_UINT: return "R8 uint"; default: return "?";
    }
}

static void vulkan_checks(void) {
    section("vulkan");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "thor_probe", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst; VkResult vr = vkCreateInstance(&ici, NULL, &inst);
    if (vr) { printf("  vkCreateInstance failed: %d\n", vr); return; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice devs[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, devs);
    printf("  physical devices: %u\n", n);
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDevice pd = devs[d];
        VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceSubgroupProperties sub = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &drv };
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, &sub };
        VkPhysicalDeviceTransformFeedbackPropertiesEXT tfp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT, &hostp };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, NULL };
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
        exts = calloc(ne, sizeof *exts); next = ne; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, exts);
        /* only chain property structs of extensions that are present */
        void *chain = &sub; drv.pNext = NULL;
        if (has_ext(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) { hostp.pNext = chain; chain = &hostp; }
        if (has_ext(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) { tfp.pNext = chain; chain = &tfp; }
        p2.pNext = chain; vkGetPhysicalDeviceProperties2(pd, &p2);
        VkPhysicalDeviceProperties *p = &p2.properties; VkPhysicalDeviceLimits *L = &p->limits;
        printf("  device %u: %s, type %d, Vulkan %u.%u.%u, driver %s (%s), vendor %#x device %#x, raw driver version %#x\n", d, p->deviceName, p->deviceType,
               VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion), drv.driverName, drv.driverInfo,
               p->vendorID, p->deviceID, p->driverVersion);
        printf("  subgroup size %u, %u extensions\n", sub.subgroupSize, ne);

        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
        VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
        vkGetPhysicalDeviceFeatures2(pd, &f2); VkPhysicalDeviceFeatures *f = &f2.features;
        section("core features");
        yesno("textureCompressionBC", f->textureCompressionBC); yesno("textureCompressionASTC_LDR", f->textureCompressionASTC_LDR);
        yesno("textureCompressionETC2", f->textureCompressionETC2); yesno("geometryShader", f->geometryShader);
        yesno("tessellationShader", f->tessellationShader); yesno("shaderClipDistance", f->shaderClipDistance);
        yesno("shaderCullDistance", f->shaderCullDistance); yesno("depthClamp", f->depthClamp); yesno("depthBiasClamp", f->depthBiasClamp);
        yesno("depthBounds", f->depthBounds); yesno("fillModeNonSolid", f->fillModeNonSolid); yesno("wideLines", f->wideLines);
        yesno("largePoints", f->largePoints); yesno("logicOp", f->logicOp); yesno("dualSrcBlend", f->dualSrcBlend);
        yesno("independentBlend", f->independentBlend); yesno("multiViewport", f->multiViewport); yesno("samplerAnisotropy", f->samplerAnisotropy);
        yesno("fragmentStoresAndAtomics", f->fragmentStoresAndAtomics); yesno("vertexPipelineStoresAndAtomics", f->vertexPipelineStoresAndAtomics);
        yesno("shaderStorageImageWriteWithoutFormat", f->shaderStorageImageWriteWithoutFormat);
        yesno("shaderStorageImageReadWithoutFormat", f->shaderStorageImageReadWithoutFormat);
        yesno("shaderFloat64", f->shaderFloat64); yesno("shaderInt64", f->shaderInt64); yesno("shaderInt16", f->shaderInt16);
        yesno("imageCubeArray", f->imageCubeArray); yesno("multiDrawIndirect", f->multiDrawIndirect); yesno("drawIndirectFirstInstance", f->drawIndirectFirstInstance);
        yesno("occlusionQueryPrecise", f->occlusionQueryPrecise); yesno("sampleRateShading", f->sampleRateShading);
        yesno("shaderImageGatherExtended", f->shaderImageGatherExtended); yesno("variableMultisampleRate", f->variableMultisampleRate);
        yesno("robustBufferAccess", f->robustBufferAccess);
        section("1.1 / 1.2 / 1.3 features");
        yesno("shaderDrawParameters", f11.shaderDrawParameters); yesno("storageBuffer16BitAccess", f11.storageBuffer16BitAccess);
        yesno("shaderOutputLayer", f12.shaderOutputLayer); yesno("shaderOutputViewportIndex", f12.shaderOutputViewportIndex);
        yesno("bufferDeviceAddress", f12.bufferDeviceAddress); yesno("descriptorIndexing", f12.descriptorIndexing);
        yesno("runtimeDescriptorArray", f12.runtimeDescriptorArray); yesno("timelineSemaphore", f12.timelineSemaphore);
        yesno("shaderFloat16", f12.shaderFloat16); yesno("shaderInt8", f12.shaderInt8); yesno("scalarBlockLayout", f12.scalarBlockLayout);
        yesno("samplerFilterMinmax", f12.samplerFilterMinmax); yesno("drawIndirectCount", f12.drawIndirectCount);
        yesno("hostQueryReset", f12.hostQueryReset); yesno("samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge);
        yesno("vulkanMemoryModel", f12.vulkanMemoryModel); yesno("uniformBufferStandardLayout", f12.uniformBufferStandardLayout);
        yesno("dynamicRendering", f13.dynamicRendering); yesno("synchronization2", f13.synchronization2); yesno("maintenance4", f13.maintenance4);
        yesno("shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation); yesno("inlineUniformBlock", f13.inlineUniformBlock);
        yesno("robustImageAccess", f13.robustImageAccess);

        section("extensions the backend cares about");
        static const char *want[] = {
            VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
            VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
            VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
            VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME,
            VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME,
            VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME, VK_EXT_4444_FORMATS_EXTENSION_NAME, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
            VK_EXT_MULTI_DRAW_EXTENSION_NAME, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
            VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
            VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
        };
        for (size_t i = 0; i < sizeof want / sizeof *want; i++) yesno(want[i], has_ext(want[i]));
        if (has_ext(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME))
            printf("  transform feedback: %u streams, %u buffers, query %u, draw indirect %u\n", tfp.maxTransformFeedbackStreams, tfp.maxTransformFeedbackBuffers,
                   tfp.transformFeedbackQueries, tfp.transformFeedbackDraw);
        if (has_ext(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) printf("  host memory import alignment %#" PRIx64 "\n", (uint64_t)hostp.minImportedHostPointerAlignment);

        section("formats (sampled / color attachment / blend / storage / depth)");
        static const VkFormat fmts[] = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
            VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
            VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_R8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_S8_UINT };
        for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fmts[i], &fp); VkFormatFeatureFlags o = fp.optimalTilingFeatures;
            printf("  %-16s sampled %-3s linear-filter %-3s color %-3s blend %-3s storage %-3s depth %-3s\n", fmt_name(fmts[i]),
                   o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "yes" : "no", o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "yes" : "no",
                   o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "yes" : "no", o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? "yes" : "no",
                   o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "yes" : "no", o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "yes" : "no");
        }

        section("limits");
        printf("  image 2D %u, 3D %u, cube %u, layers %u, framebuffer layers %u, color attachments %u, viewports %u\n", L->maxImageDimension2D,
               L->maxImageDimension3D, L->maxImageDimensionCube, L->maxImageArrayLayers, L->maxFramebufferLayers, L->maxColorAttachments, L->maxViewports);
        printf("  storage buffer range %u, uniform buffer range %u, push constants %u, bound sets %u\n", L->maxStorageBufferRange,
               L->maxUniformBufferRange, L->maxPushConstantsSize, L->maxBoundDescriptorSets);
        printf("  per stage: samplers %u, sampled images %u, storage buffers %u, uniform buffers %u, storage images %u, resources %u\n",
               L->maxPerStageDescriptorSamplers, L->maxPerStageDescriptorSampledImages, L->maxPerStageDescriptorStorageBuffers,
               L->maxPerStageDescriptorUniformBuffers, L->maxPerStageDescriptorStorageImages, L->maxPerStageResources);
        printf("  vertex attributes %u, bindings %u, stride %u; uniform offset align %" PRIu64 ", storage offset align %" PRIu64 ", texel buffer elements %u\n",
               L->maxVertexInputAttributes, L->maxVertexInputBindings, L->maxVertexInputBindingStride, (uint64_t)L->minUniformBufferOffsetAlignment,
               (uint64_t)L->minStorageBufferOffsetAlignment, L->maxTexelBufferElements);
        printf("  compute: group size %u x %u x %u, invocations %u, shared memory %u; timestamp period %.2f ns, point size range %.0f..%.0f\n",
               L->maxComputeWorkGroupSize[0], L->maxComputeWorkGroupSize[1], L->maxComputeWorkGroupSize[2], L->maxComputeWorkGroupInvocations,
               L->maxComputeSharedMemorySize, L->timestampPeriod, L->pointSizeRange[0], L->pointSizeRange[1]);

        section("memory");
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++) printf("  heap %u: %" PRIu64 " MiB flags %#x\n", i, (uint64_t)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags fl = mp.memoryTypes[i].propertyFlags;
            printf("  type %u heap %u:%s%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex, fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
                   fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "", fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
                   fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "", fl & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " lazy" : "");
        }

        /* can a device be created with the features the backend would ask for? */
        VkPhysicalDeviceFeatures2 req = f2; f13.pNext = NULL;
        float prio = 1.0f; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &req, 0, 1, &q, 0, NULL, 0, NULL, NULL };
        VkDevice dev; vr = vkCreateDevice(pd, &dci, NULL, &dev);
        printf("\n  vkCreateDevice with every supported 1.0-1.3 feature: %s (%d)\n", vr ? "FAILED" : "ok", vr);
        if (!vr) vkDestroyDevice(dev, NULL);
        free(exts); exts = NULL; next = 0;
    }
    vkDestroyInstance(inst, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("thor_probe (THOR_SCOPE.md step T0)\n");
    host_checks();
    vulkan_checks();
    return 0;
}
