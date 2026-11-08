// From metroid-prime-4-recomp runtime/vk_caps.c @ 586e91e1b6f38c9e512e39f2d47a046fdf273cf0 (MIT, Copyright (c) 2026 Jeff Aigner), copied with mp4 renamed to oot.
// From fe3h-pc-recomp runtime/vk_caps.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only.
// From totk-pc-recomp runtime/vk_caps.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only.
// What the runtime branches on, logged once at device creation, and the fallbacks for GPUs other than the RX 9070.
//
// Fallbacks (each also forced on any GPU by an environment switch, to test it where the GPU does not need it):
//   - no shaderFloat64 (OOT_VK_NO_FLOAT64=1 clears the feature before vkCreateDevice): the translator lowers FP64 to
//     FP32 ("softfloat64"), so no shader declares Float64.
//   - minStorageBufferOffsetAlignment above 4 (OOT_VK_SSBO_ALIGN=<power of two> raises it): the translator computes
//     storage offsets from the guest address rounded down to that alignment (Ryujinx's unaligned storage path), and
//     vk_exec_draw.c ssbo_info binds the rounded-down address, so shader writes reach guest memory instead of a copy.
//   - no graphics pipeline library with fast linking (OOT_GPL=0 forces it; the default on AMD's RDNA 1/2 driver):
//     vk_caps.gpl = 0, the extension stays disabled, and pipelines are built whole.
//   - no 32-lane pin (no subgroupSizeControl, 32 outside min..max, or OOT_VK_SUBGROUP_PIN=0): stages run at the
//     device's subgroupSize (SPIR-V 1.3 without a required size) and the translator is told that size.
//     OOT_VK_SUBGROUP=<n> pins n instead of 32 where the driver allows it.
// The defaults (Float64, no storage rounding, 32 everywhere) leave the shader cache keys as they were (gfx_shader.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"

// SM86 HMMA on the tensor cores (sm86_lower.c lower_hmma, tools/xlate patch 0016): VK_KHR_cooperative_matrix, half
// floats in shaders, and compute subgroups pinned to 32 lanes (the fragments are per lane of a 32-lane warp). Two
// ways, by the shapes the device reports (OOT_TENSOR_CORES=0 keeps the shuffle emulation):
//   1  f16 16x8x16 subgroup matrices with f16 accumulators (NVIDIA): HMMA's own shape, its register fragments are
//      the matrices' elements (measured on the RTX 4070 with --probe-coopmat).
//   2  otherwise f16 16x16x16 with f32 accumulators (AMD), if the element order measured here at every start
//      (coopmat_probe.c coopmat_layout_measure, about 16 ms) is the one the translator's 16x16 mapping assumes, the
//      RX 9070 XT's (RDNA 4): B and the result padded from 8 columns to 16, the fragments moved between lanes with
//      subgroup shuffles, C added after the product in f32 (tools/xlate patch 0027, docs/phase3.md 2026-11-08). A
//      device with another order stays emulated.
static int tensor_cores(VkCtx *c, int sg32) {
    const char *e = cfg_get("OOT_TENSOR_CORES");
    if (e && *e && !atoi(e)) { rt_log("vk caps: tensor cores off (OOT_TENSOR_CORES=0): HMMA emulated"); return 0; }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    int shape = 0, shape16 = 0;
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get = c->has_coopmat ?
        (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)vkGetInstanceProcAddr(
            c->instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR") : NULL;
    uint32_t n = 0;
    if (get) get(c->phys, &n, NULL);
    VkCooperativeMatrixPropertiesKHR *p = n ? calloc(n, sizeof *p) : NULL;
    for (uint32_t i = 0; p && i < n; i++) p[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    if (p) get(c->phys, &n, p);
    for (uint32_t i = 0; p && i < n; i++) {
        shape |= p[i].MSize == 16 && p[i].NSize == 8 && p[i].KSize == 16 && p[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                 p[i].BType == VK_COMPONENT_TYPE_FLOAT16_KHR && p[i].CType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                 p[i].ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR && p[i].scope == VK_SCOPE_SUBGROUP_KHR;
        shape16 |= p[i].MSize == 16 && p[i].NSize == 16 && p[i].KSize == 16 && p[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                   p[i].BType == VK_COMPONENT_TYPE_FLOAT16_KHR && p[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
                   p[i].ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR && p[i].scope == VK_SCOPE_SUBGROUP_KHR &&
                   !p[i].saturatingAccumulation;
    }
    free(p);
    int base = c->has_coopmat && f12.shaderFloat16 && sg32;
    if (base && shape) {
        rt_log("vk caps: tensor cores on: HMMA as cooperative matrix multiplies (cooperative matrix 1, f16 16x8x16 1, "
               "shaderFloat16 1, compute subgroups of 32 1)");
        return 1;
    }
    if (base && shape16) {
        // What the measuring shader (16-bit storage, the Vulkan memory model) and full subgroups need: the pipelines
        // of mode 2 are created with REQUIRE_FULL_SUBGROUPS (vk_pipeline.c compute_create), as Vulkan asks of
        // cooperative matrices in SPIR-V before 1.6, and the shuffles read lanes by number.
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
        VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &cm};
        f12.pNext = &f11;
        vkGetPhysicalDeviceFeatures2(c->phys, &f2);
        if (!cm.cooperativeMatrix || !f11.storageBuffer16BitAccess || !f12.vulkanMemoryModel || !vk_caps.full_subgroups) {
            rt_log("vk caps: tensor cores off: HMMA emulated (f16 16x16x16 matrices, but cooperativeMatrix %d, "
                   "storageBuffer16BitAccess %d, vulkanMemoryModel %d, computeFullSubgroups %d)", (int)cm.cooperativeMatrix,
                   (int)f11.storageBuffer16BitAccess, (int)f12.vulkanMemoryModel, vk_caps.full_subgroups);
            return 0;
        }
        CoopmatLayout L;
        const char *why = NULL;
        ULONGLONG t0 = GetTickCount64();
        int measured = coopmat_layout_measure(c, &L, &why), ok = measured && coopmat_layout_16x16_ok(&L);
        if (ok) {
            rt_log("vk caps: tensor cores on: HMMA as f16 16x16x16 cooperative matrix multiplies with f32 accumulators, "
                   "fragments moved by subgroup shuffles (element order measured in %llu ms)", GetTickCount64() - t0);
            return 2;
        }
        rt_log("vk caps: tensor cores off: HMMA emulated (f16 16x16x16 matrices, but %s; oot.exe --probe-coopmat prints it)",
               measured ? "their element order is not the one the translator's 16x16 mapping assumes" : why);
        return 0;
    }
    rt_log("vk caps: tensor cores off: HMMA emulated (cooperative matrix %d, f16 16x8x16 %d, f16 16x16x16 f32 %d, "
           "shaderFloat16 %d, compute subgroups of 32 %d)", c->has_coopmat, shape, shape16, (int)f12.shaderFloat16, sg32);
    return 0;
}

void vk_caps_tensor(VkCtx *c) {
    vk_caps.tensor = tensor_cores(c, vk_caps.sg_size == 32 && (vk_caps.sg_pin_stages & VK_SHADER_STAGE_COMPUTE_BIT));
    gfx_shader_set_tensor(vk_caps.tensor);
}

VkCaps vk_caps = {.float64 = 1, .sg_size = 32};
static int f64_supported;
static VkBool32 gpl_lib, gpl_fast;   // the driver's graphicsPipelineLibrary (feature), ...FastLinking (property)

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *e = cfg_get(name);
    return e && *e ? (uint32_t)strtoul(e, NULL, 0) : def;
}

void vk_caps_features(VkPhysicalDeviceFeatures *f) {
    f64_supported = f->shaderFloat64;
    if (env_u32("OOT_VK_NO_FLOAT64", 0)) f->shaderFloat64 = VK_FALSE;
    vk_caps.float64 = f->shaderFloat64;
}

void vk_caps_driver(VkPhysicalDevice p) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(p, &p2);
    vk_caps.amd = p2.properties.vendorID == 0x1002;
    // AMD split RDNA 1 and 2 off into a driver branch of their own; its driverInfo reads
    // "26.10.1 (AMD proprietary shader compiler)" where the RDNA 3 and 4 driver names LLPC.
    vk_caps.amd_sc = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY && strstr(drv.driverInfo, "shader compiler");
}

// OOT_GPL=0/1 forces graphics pipeline libraries off or on. Default off on AMD's RDNA 1/2 driver branch: every boot
// that lost the device there had a cold pipeline cache and draws waiting on fast-linked pipelines, and a run with GPL
// off booted (issue #5); Savage Wind Recomp turns GPL off on RDNA 4 under RADV for a GPU hang at the title screen.
int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p) {
    gpl_lib = f->graphicsPipelineLibrary;
    gpl_fast = p->graphicsPipelineLibraryFastLinking;
    const char *e = cfg_get("OOT_GPL");   // no fixed default: it depends on the driver, see above
    vk_caps.gpl = gpl_lib && gpl_fast && (e && *e ? strcmp(e, "0") != 0 : !vk_caps.amd_sc);
    return vk_caps.gpl;
}

static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static char depth_ok(VkPhysicalDevice p, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(p, f, &fp);
    return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'y' : 'n';
}

static void driver_version(char *o, size_t n, uint32_t vendor, uint32_t v) {
    if (vendor == 0x10de) snprintf(o, n, "%u.%u.%u.%u", v >> 22, v >> 14 & 0xff, v >> 6 & 0xff, v & 0x3f);
    else if (vendor == 0x8086) snprintf(o, n, "%u.%u", v >> 14, v & 0x3fff);
    else snprintf(o, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void vk_caps_init(VkCtx *c) {
    VkPhysicalDeviceLimits *l = &c->props.limits;
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &sg};
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
                                                      .pNext = &p13};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? &pvf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, ep);

    uint32_t real_align = (uint32_t)l->minStorageBufferOffsetAlignment, a = env_u32("OOT_VK_SSBO_ALIGN", 0);
    if (a && !(a & (a - 1)) && a > real_align) l->minStorageBufferOffsetAlignment = a;
    vk_caps.sbuf_align = l->minStorageBufferOffsetAlignment > 4 ? (uint32_t)l->minStorageBufferOffsetAlignment : 0;

    uint32_t want = env_u32("OOT_VK_SUBGROUP", 32);
    int pin = f13.subgroupSizeControl && cfg_bool("OOT_VK_SUBGROUP_PIN") && p13.minSubgroupSize <= want &&
              want <= p13.maxSubgroupSize;
    vk_caps.sg_size = want;
    vk_caps.sg_pin_stages = pin ? p13.requiredSubgroupSizeStages : 0;
    vk_caps.full_subgroups = f13.computeFullSubgroups;   // enabled with the rest of the chain (vk_device.c)
    static const VkShaderStageFlags stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    uint32_t told[3];
    for (int i = 0; i < 3; i++) told[i] = vk_caps.sg_pin_stages & stage[i] ? want : sg.subgroupSize;
    gfx_shader_set_host(vk_caps.float64, vk_caps.sbuf_align, told);

    char dv[32];
    driver_version(dv, sizeof dv, c->props.vendorID, c->props.driverVersion);
    uint32_t v = c->props.apiVersion;
    VkPhysicalDevice p = c->phys;
    rt_log("vk caps: %s, vendor %#06x device %#06x, driver %s \"%s\" (id %d, version %s, raw %#x), api %u.%u.%u",
           c->props.deviceName, c->props.vendorID, c->props.deviceID, drv.driverName, drv.driverInfo, (int)drv.driverID, dv,
           c->props.driverVersion, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    rt_log("vk caps: subgroup size %u, stages %#x, ops %#x; size control %d (%u-%u, required stages %#x)", sg.subgroupSize,
           (unsigned)sg.supportedStages, (unsigned)sg.supportedOperations, (int)f13.subgroupSizeControl, p13.minSubgroupSize,
           p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages);
    rt_log("vk caps: shaderFloat64 %d (enabled %d), storage buffer offset alignment %u (device %u), uniform %llu, "
           "maxPushDescriptors %u, provoking vertex ext %d last %d",
           f64_supported, vk_caps.float64, (unsigned)l->minStorageBufferOffsetAlignment, real_align,
           (unsigned long long)l->minUniformBufferOffsetAlignment, pd.maxPushDescriptors, c->has_provoking_vertex,
           (int)pvf.provokingVertexLast);
    rt_log("vk caps: ext index_type_uint8 khr %d ext %d, attachment_feedback_loop_layout %d dynamic_state %d, "
           "graphics_pipeline_library %d; depth attachment D16 %c D24S8 %c X8D24 %c D32 %c D32S8 %c",
           has_ext(ep, ne, "VK_KHR_index_type_uint8"), has_ext(ep, ne, "VK_EXT_index_type_uint8"),
           has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_layout"), has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_dynamic_state"),
           has_ext(ep, ne, "VK_EXT_graphics_pipeline_library"), depth_ok(p, VK_FORMAT_D16_UNORM),
           depth_ok(p, VK_FORMAT_D24_UNORM_S8_UINT), depth_ok(p, VK_FORMAT_X8_D24_UNORM_PACK32), depth_ok(p, VK_FORMAT_D32_SFLOAT),
           depth_ok(p, VK_FORMAT_D32_SFLOAT_S8_UINT));
    const char *ge = cfg_get("OOT_GPL");
    rt_log("vk caps: gpl %s fast-link %s (graphicsPipelineLibrary %u%s)", vk_caps.gpl ? "yes" : "no",
           gpl_fast ? "yes" : "no", (unsigned)gpl_lib,
           !gpl_lib || !gpl_fast || vk_caps.gpl ? "" : ge && *ge ? ", off by OOT_GPL=0" : ", off on AMD's RDNA 1/2 driver");
    rt_log("vk caps: fallbacks: float64 %s, storage buffers %s %u, subgroup pin %u on stages %#x, translator subgroup vs/fs/cs %u/%u/%u",
           vk_caps.float64 ? "native" : "softfloat64", vk_caps.sbuf_align ? "rounded down to" : "exact, alignment",
           (unsigned)l->minStorageBufferOffsetAlignment, want, (unsigned)vk_caps.sg_pin_stages, told[0], told[1], told[2]);
    free(ep);
}
