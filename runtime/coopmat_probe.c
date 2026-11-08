// Written for ocarina-of-time-recomp (MIT, Copyright (c) 2026 Jeff Aigner).
//
// oot.exe --probe-coopmat [shader.spv]: whether VK_KHR_cooperative_matrix can stand in for SM86's HMMA.16816.F16
// (sm86_lower.c lower_hmma). Lists the matrix shapes the device reports, then measures the element order of its 16x16
// subgroup matrices (coopmat_layout_measure, which vk_caps.c also runs at start-up) and says whether it is the order
// the translator's 16x16 mapping of HMMA assumes (coopmat_layout_16x16_ok).
//
// With a shader, it also runs that one-subgroup compute shader (scratch GLSL, compiled by hand; written for NVIDIA's
// 16x8x16 shape) that fills a 16x16 A, a 16x8 B and a 16x8 accumulator element by element from each lane's words, in
// the order HMMA keeps them in its registers, multiplies, and writes D back the same way. The result is compared with
// the product computed on the CPU from the HMMA fragment layout (PTX ISA, mma.m16n8k16 .f16):
//   lane = 4g + t; A regs: (A[g][2t], A[g][2t+1]), (A[g+8][2t], ..), (A[g][2t+8], ..), (A[g+8][2t+8], ..);
//   B regs: (B[2t][g], B[2t+1][g]), (B[2t+8][g], B[2t+9][g]); C and D: (C[g][2t], C[g][2t+1]), (C[g+8][2t], ..).
// If the element order of the device's subgroup matrices is that layout, an HMMA becomes a matrix multiply with no
// data movement between lanes. Buffer: a[128] b[64] c[64] d[64] len[4] words, binding 0.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "coopmat_layout_spv.h"
#include "host.h"
#include "vk.h"

static uint16_t f2h(float f) {   // values are small multiples of 1/4: exact
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t sign = (u >> 16) & 0x8000;
    int exp = (int)((u >> 23) & 0xff) - 127 + 15;
    if (f == 0 || exp <= 0) return (uint16_t)sign;
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((u & 0x7fffff) >> 13));
}

static float h2f(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, u;
    if (exp == 0) u = sign;
    else u = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t pack(float lo, float hi) { return (uint32_t)f2h(lo) | (uint32_t)f2h(hi) << 16; }

static const char *type_name(VkComponentTypeKHR t) {
    switch (t) {
    case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
    case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
    case VK_COMPONENT_TYPE_SINT8_KHR: return "s8";
    case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
    case VK_COMPONENT_TYPE_SINT32_KHR: return "s32";
    case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
    default: return "other";
    }
}

// One dispatch of a single workgroup of `code` with one storage buffer at binding 0, filled from `data` and read back
// into it. Compute subgroups are pinned to 32 lanes where the game pins them (vk_caps.sg_pin_stages), so the matrices
// are distributed as in the game's kernels; full (a workgroup of whole subgroups, computeFullSubgroups) also asks for
// full subgroups, as the game's mode-2 pipelines do (vk_pipeline.c compute_create). Needs the device and the allocator
// only (vk_caps.c runs it before vk_init returns). 0 and a reason on failure.
static int run_one(VkCtx *c, const uint32_t *code, size_t code_bytes, void *data, size_t bytes, int full, const char **why) {
    int ok = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation mem = NULL;
    VmaAllocationInfo info;
    VkShaderModule sm = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkCommandPool cp = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    *why = "buffer";
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) return 0;
    memcpy(info.pMappedData, data, bytes);
    vmaFlushAllocation(c->vma, mem, 0, VK_WHOLE_SIZE);

    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_bytes, .pCode = code};
    *why = "shader module";
    if (vkCreateShaderModule(c->device, &mi, NULL, &sm) != VK_SUCCESS) goto done;
    VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
                                          .pBindings = &b0};
    vkCreateDescriptorSetLayout(c->device, &li, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl};
    vkCreatePipelineLayout(c->device, &pli, NULL, &pl);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vk_caps.sg_size};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .pNext = vk_caps.sg_pin_stages & VK_SHADER_STAGE_COMPUTE_BIT ? &rs : NULL,
                                                .flags = full && vk_caps.full_subgroups && (vk_caps.sg_pin_stages & VK_SHADER_STAGE_COMPUTE_BIT) ?
                                                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    *why = "pipeline";
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) != VK_SUCCESS) goto done;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    vkCreateDescriptorPool(c->device, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(c->device, &dai, &ds);
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(c->device, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->gfx_family};
    vkCreateCommandPool(c->device, &cpi, NULL, &cp);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(c->device, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(c->device, &fi, NULL, &fence);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();
    VkResult qr = vkQueueSubmit(c->gfx_queue, 1, &si, fence);
    vk_queue_unlock();
    *why = "submit or wait";
    if (qr != VK_SUCCESS || vkWaitForFences(c->device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) goto done;
    vmaInvalidateAllocation(c->vma, mem, 0, VK_WHOLE_SIZE);
    memcpy(data, info.pMappedData, bytes);
    ok = 1, *why = NULL;
done:
    if (fence) vkDestroyFence(c->device, fence, NULL);
    if (cp) vkDestroyCommandPool(c->device, cp, NULL);
    if (dp) vkDestroyDescriptorPool(c->device, dp, NULL);
    if (pipe) vkDestroyPipeline(c->device, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(c->device, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(c->device, dsl, NULL);
    if (sm) vkDestroyShaderModule(c->device, sm, NULL);
    vmaDestroyBuffer(c->vma, buf, mem);
    return ok;
}

int coopmat_layout_measure(VkCtx *c, CoopmatLayout *out, const char **why) {
    enum { H = 768, F = 256 };
    struct { uint16_t h[H]; float f[F]; uint32_t len[4]; } b;
    memset(&b, 0xff, sizeof b);   // what no store reaches stays 0xffff (an f16 NaN) and NaN
    if (!run_one(c, coopmat_layout_spv, sizeof coopmat_layout_spv, &b, sizeof b, 1, why)) return 0;   // local size 32
    memcpy(out->len, b.len, sizeof out->len);
    for (int i = 0; i < 256; i++) {
        for (int m = 0; m < 3; m++) {
            float v = h2f(b.h[256 * m + i]);
            out->who[m][i] = b.h[256 * m + i] != 0xffff && v >= 0 && v < 512 && v == floorf(v) ? (uint16_t)v : 0xffff;
        }
        float v = b.f[i];
        out->who[3][i] = v >= 0 && v < 512 && v == floorf(v) ? (uint16_t)v : 0xffff;
    }
    return 1;
}

// The order the translator's 16x16 mapping assumes (tools/xlate patch 0016, GenerateMatrixMultiplyAdd16): 8 elements
// per lane in each matrix; A: lane l holds row l % 16, columns (k) 8 (l / 16) .. + 7 as elements 0-7; B: lane l holds
// column l % 16, rows (k) 8 (l / 16) .. + 7; the f32 accumulator: lane l holds column l % 16, rows 8 (l / 16) .. + 7.
// Measured on the RX 9070 XT (RDNA 4, AMD's driver, 32-lane subgroups). The f16 accumulator is not used.
static int expect_16x16(int m, int r, int col) {
    switch (m) {
    case 0: return 16 * (r + 16 * (col / 8)) + col % 8;   // A
    case 1: return 16 * (col + 16 * (r / 8)) + r % 8;     // B
    default: return 16 * (col + 16 * (r / 8)) + r % 8;    // accumulators
    }
}

int coopmat_layout_16x16_ok(const CoopmatLayout *L) {
    if (L->len[0] != 8 || L->len[1] != 8 || L->len[3] != 8) return 0;
    static const int used[] = {0, 1, 3};
    for (int u = 0; u < 3; u++)
        for (int i = 0; i < 256; i++)
            if (L->who[used[u]][i] != expect_16x16(used[u], i / 16, i % 16)) return 0;
    return 1;
}

static void print_layout(const CoopmatLayout *L) {
    static const char *name[4] = {"A f16", "B f16", "accumulator f16", "accumulator f32"};
    for (int m = 0; m < 4; m++) {
        printf("probe-coopmat: %s, %u elements per lane; row r: lane.element of columns 0-15\n", name[m], L->len[m]);
        for (int r = 0; r < 16; r++) {
            char line[16 * 7 + 1], *p = line;
            for (int col = 0; col < 16; col++) {
                uint16_t w = L->who[m][16 * r + col];
                p += w == 0xffff ? sprintf(p, "    --") : sprintf(p, " %2u.%-2u", w / 16, w % 16);
            }
            printf("probe-coopmat:   %2d%s\n", r, line);
        }
    }
}

int probe_coopmat(const char *spv_path) {
    if (!vk_init()) { printf("probe-coopmat: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("probe-coopmat: GPU %s, VK_KHR_cooperative_matrix %d\n", c->props.deviceName, c->has_coopmat);
    if (!c->has_coopmat) { printf("probe-coopmat: not supported\n"); return 1; }
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get_props =
        (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)vkGetInstanceProcAddr(
            c->instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    uint32_t n = 0;
    if (get_props) get_props(c->phys, &n, NULL);
    VkCooperativeMatrixPropertiesKHR *pr = calloc(n ? n : 1, sizeof *pr);
    for (uint32_t i = 0; i < n; i++) pr[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    if (get_props) get_props(c->phys, &n, pr);
    int have_16816 = 0, have_161616 = 0;
    for (uint32_t i = 0; i < n; i++) {
        printf("probe-coopmat:   M%u N%u K%u  A %s B %s C %s R %s  scope %d%s\n", pr[i].MSize, pr[i].NSize, pr[i].KSize,
               type_name(pr[i].AType), type_name(pr[i].BType), type_name(pr[i].CType), type_name(pr[i].ResultType),
               (int)pr[i].scope, pr[i].saturatingAccumulation ? " saturating" : "");
        if (pr[i].MSize == 16 && pr[i].NSize == 8 && pr[i].KSize == 16 && pr[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            pr[i].CType == VK_COMPONENT_TYPE_FLOAT16_KHR && pr[i].ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            pr[i].scope == VK_SCOPE_SUBGROUP_KHR)
            have_16816 = 1;
        if (pr[i].MSize == 16 && pr[i].NSize == 16 && pr[i].KSize == 16 && pr[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            pr[i].BType == VK_COMPONENT_TYPE_FLOAT16_KHR && pr[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
            pr[i].ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR && pr[i].scope == VK_SCOPE_SUBGROUP_KHR &&
            !pr[i].saturatingAccumulation)
            have_161616 = 1;
    }
    free(pr);
    printf("probe-coopmat: f16 16x8x16 subgroup matrices: %s; f16 16x16x16 with f32 accumulators: %s\n",
           have_16816 ? "yes" : "no", have_161616 ? "yes" : "no");
    printf("probe-coopmat: compute subgroups %s\n",
           vk_caps.sg_pin_stages & VK_SHADER_STAGE_COMPUTE_BIT ? "pinned to 32 lanes" : "not pinned (device default)");
    int rc = have_16816 ? 0 : 1;
    if (have_161616) {
        CoopmatLayout L;
        const char *why;
        if (!coopmat_layout_measure(c, &L, &why)) printf("probe-coopmat: FAIL measuring the 16x16 order: %s\n", why);
        else {
            print_layout(&L);
            int ok = coopmat_layout_16x16_ok(&L);
            printf("probe-coopmat: 16x16 element order: %s\n",
                   ok ? "the one the translator's 16x16 mapping of HMMA assumes" : "NOT the one the translator's 16x16 mapping assumes");
            if (!have_16816) rc = ok ? 0 : 1;
        }
    }
    if (!spv_path) return rc;

    FILE *f = fopen(spv_path, "rb");
    if (!f) { printf("probe-coopmat: FAIL cannot open %s\n", spv_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)sz);
    if (fread(code, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); printf("probe-coopmat: FAIL reading the shader\n"); return 1; }
    fclose(f);

    // Inputs in HMMA register order, from matrices of small values (exact in f16 and in every partial sum).
    float A[16][16], B[16][8], C[16][8], D[16][8];
    uint32_t rng = 12345;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++) rng = rng * 1664525u + 1013904223u, A[i][k] = (float)((int)(rng >> 28) - 8) / 4;
    for (int k = 0; k < 16; k++)
        for (int j = 0; j < 8; j++) rng = rng * 1664525u + 1013904223u, B[k][j] = (float)((int)(rng >> 28) - 8) / 4;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++) rng = rng * 1664525u + 1013904223u, C[i][j] = (float)((int)(rng >> 28) - 8);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++) {
            float s = C[i][j];
            for (int k = 0; k < 16; k++) s += A[i][k] * B[k][j];
            D[i][j] = s;
        }
    enum { WORDS = 128 + 64 + 64 + 64 + 4 };
    uint32_t out[WORDS];
    memset(out, 0, sizeof out);
    for (int l = 0; l < 32; l++) {
        int g = l / 4, t = l % 4;
        out[l * 4 + 0] = pack(A[g][2 * t], A[g][2 * t + 1]);
        out[l * 4 + 1] = pack(A[g + 8][2 * t], A[g + 8][2 * t + 1]);
        out[l * 4 + 2] = pack(A[g][2 * t + 8], A[g][2 * t + 9]);
        out[l * 4 + 3] = pack(A[g + 8][2 * t + 8], A[g + 8][2 * t + 9]);
        out[128 + l * 2 + 0] = pack(B[2 * t][g], B[2 * t + 1][g]);
        out[128 + l * 2 + 1] = pack(B[2 * t + 8][g], B[2 * t + 9][g]);
        out[192 + l * 2 + 0] = pack(C[g][2 * t], C[g][2 * t + 1]);
        out[192 + l * 2 + 1] = pack(C[g + 8][2 * t], C[g + 8][2 * t + 1]);
    }
    const char *why;
    int ran = run_one(c, code, (size_t)sz, out, sizeof out, 0, &why);
    free(code);
    if (!ran) { printf("probe-coopmat: FAIL %s\n", why); return 1; }
    FILE *raw = fopen("build/logs/coopmat-out.bin", "wb");   // the buffer after the run, for shaders other than the test's
    if (raw) fwrite(out, 4, WORDS, raw), fclose(raw);
    printf("probe-coopmat: elements per lane: A %u, B %u, C %u, D %u\n", out[320], out[321], out[322], out[323]);
    int bad = 0;
    for (int l = 0; l < 32; l++) {
        int g = l / 4, t = l % 4;
        float want[4] = {D[g][2 * t], D[g][2 * t + 1], D[g + 8][2 * t], D[g + 8][2 * t + 1]};
        for (int e = 0; e < 4; e++) {
            float got = h2f((uint16_t)(out[256 + l * 2 + e / 2] >> (16 * (e & 1))));
            if (got != want[e]) {
                if (bad < 8) printf("probe-coopmat:   lane %d element %d: %g, HMMA layout wants %g\n", l, e, got, want[e]);
                bad++;
            }
        }
    }
    printf("probe-coopmat: %s (%d of 128 result elements differ from the HMMA layout)\n",
           bad ? "FAIL" : "the subgroup matrix element order is HMMA's register order", bad);
    return bad ? 1 : 0;
}
