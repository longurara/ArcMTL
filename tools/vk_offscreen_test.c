/*
 * tools/vk_offscreen_test.c — Stage 2 Mesa ANV Vulkan 1.3 Offscreen GPU Verification
 * SPDX-License-Identifier: MIT
 *
 * Loads libvulkan_intel.dylib directly via dlopen (no external Vulkan loader needed),
 * creates a VkDevice on Intel Arc (8086:7D55), dispatches vkCmdFillBuffer on Xe-LPG,
 * synchronizes via VkFence, and verifies all 64 KiB in coherent UMA memory.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK_VK(expr)                                                                  \
    do {                                                                                \
        VkResult _r = (expr);                                                           \
        if (_r != VK_SUCCESS) {                                                         \
            fprintf(stderr, "[FAIL] %s returned VkResult=%d at line %d\n",              \
                    #expr, (int)_r, __LINE__);                                          \
            return 10;                                                                  \
        }                                                                               \
    } while (0)

static const uint32_t k_b4_vs_spv[227] = {
    0x07230203u, 0x00010500u, 0x00000000u, 0x00000028u, 0x00000000u, 0x00020011u,
    0x00000001u, 0x00020011u, 0x0000000bu, 0x00020011u, 0x000014e3u, 0x0003000eu,
    0x000014e4u, 0x00000001u, 0x0009000fu, 0x00000000u, 0x0000001bu, 0x6e69616du,
    0x00000000u, 0x00000010u, 0x00000011u, 0x00000012u, 0x00000013u, 0x00040047u,
    0x00000010u, 0x0000000bu, 0x0000002au, 0x00040047u, 0x00000011u, 0x0000000bu,
    0x00000000u, 0x00040047u, 0x00000012u, 0x0000001eu, 0x00000000u, 0x00030047u,
    0x00000008u, 0x00000002u, 0x00050048u, 0x00000008u, 0x00000000u, 0x00000023u,
    0x00000000u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu,
    0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000013u, 0x00000022u,
    0x00000000u, 0x00040047u, 0x00000013u, 0x00000021u, 0x00000000u, 0x00020013u,
    0x00000001u, 0x00030021u, 0x00000002u, 0x00000001u, 0x00040015u, 0x00000003u,
    0x00000020u, 0x00000001u, 0x00040015u, 0x00000004u, 0x00000040u, 0x00000000u,
    0x00020014u, 0x00000005u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
    0x00000007u, 0x00000006u, 0x00000004u, 0x0003001eu, 0x00000008u, 0x00000007u,
    0x00040020u, 0x00000009u, 0x000014e5u, 0x00000008u, 0x00040020u, 0x0000000au,
    0x000014e5u, 0x00000007u, 0x0003001eu, 0x0000000bu, 0x00000004u, 0x00040020u,
    0x0000000cu, 0x00000002u, 0x0000000bu, 0x00040020u, 0x0000000du, 0x00000002u,
    0x00000004u, 0x00040020u, 0x0000000eu, 0x00000001u, 0x00000003u, 0x00040020u,
    0x0000000fu, 0x00000003u, 0x00000007u, 0x0004003bu, 0x0000000eu, 0x00000010u,
    0x00000001u, 0x0004003bu, 0x0000000fu, 0x00000011u, 0x00000003u, 0x0004003bu,
    0x0000000fu, 0x00000012u, 0x00000003u, 0x0004003bu, 0x0000000cu, 0x00000013u,
    0x00000002u, 0x0004002bu, 0x00000003u, 0x00000014u, 0x00000000u, 0x0004002bu,
    0x00000003u, 0x00000015u, 0x00000001u, 0x0004002bu, 0x00000003u, 0x00000016u,
    0x00000002u, 0x0004002bu, 0x00000006u, 0x00000017u, 0xbf800000u, 0x0004002bu,
    0x00000006u, 0x00000018u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000019u,
    0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000001au, 0x40400000u, 0x00050036u,
    0x00000001u, 0x0000001bu, 0x00000000u, 0x00000002u, 0x000200f8u, 0x0000001cu,
    0x0004003du, 0x00000003u, 0x0000001du, 0x00000010u, 0x000500aau, 0x00000005u,
    0x0000001eu, 0x0000001du, 0x00000015u, 0x000500aau, 0x00000005u, 0x0000001fu,
    0x0000001du, 0x00000016u, 0x000600a9u, 0x00000006u, 0x00000020u, 0x0000001eu,
    0x0000001au, 0x00000017u, 0x000600a9u, 0x00000006u, 0x00000021u, 0x0000001fu,
    0x0000001au, 0x00000017u, 0x00070050u, 0x00000007u, 0x00000022u, 0x00000020u,
    0x00000021u, 0x00000018u, 0x00000019u, 0x0003003eu, 0x00000011u, 0x00000022u,
    0x00050041u, 0x0000000du, 0x00000023u, 0x00000013u, 0x00000014u, 0x0004003du,
    0x00000004u, 0x00000024u, 0x00000023u, 0x00040078u, 0x00000009u, 0x00000025u,
    0x00000024u, 0x00050041u, 0x0000000au, 0x00000026u, 0x00000025u, 0x00000014u,
    0x0006003du, 0x00000007u, 0x00000027u, 0x00000026u, 0x00000002u, 0x00000010u,
    0x0003003eu, 0x00000012u, 0x00000027u, 0x000100fdu, 0x00010038u,
};

static const uint32_t k_b4_fs_spv[180] = {
    0x07230203u, 0x00010500u, 0x00000000u, 0x00000020u, 0x00000000u, 0x00020011u,
    0x00000001u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0009000fu, 0x00000004u,
    0x00000017u, 0x6e69616du, 0x00000000u, 0x0000000fu, 0x00000010u, 0x00000011u,
    0x00000012u, 0x00030010u, 0x00000017u, 0x00000007u, 0x00040047u, 0x0000000fu,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x00000010u, 0x0000001eu, 0x00000000u,
    0x00030047u, 0x00000009u, 0x00000002u, 0x00050048u, 0x00000009u, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040047u, 0x00000012u, 0x00000022u, 0x00000000u,
    0x00040047u, 0x00000012u, 0x00000021u, 0x00000001u, 0x00020013u, 0x00000001u,
    0x00030021u, 0x00000002u, 0x00000001u, 0x00040015u, 0x00000003u, 0x00000020u,
    0x00000001u, 0x00030016u, 0x00000004u, 0x00000020u, 0x00040017u, 0x00000005u,
    0x00000004u, 0x00000002u, 0x00040017u, 0x00000006u, 0x00000004u, 0x00000004u,
    0x00090019u, 0x00000007u, 0x00000004u, 0x00000001u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu, 0x00000008u, 0x00000007u,
    0x0003001eu, 0x00000009u, 0x00000006u, 0x00040020u, 0x0000000au, 0x00000009u,
    0x00000009u, 0x00040020u, 0x0000000bu, 0x00000009u, 0x00000006u, 0x00040020u,
    0x0000000cu, 0x00000000u, 0x00000008u, 0x00040020u, 0x0000000du, 0x00000001u,
    0x00000006u, 0x00040020u, 0x0000000eu, 0x00000003u, 0x00000006u, 0x0004003bu,
    0x0000000du, 0x0000000fu, 0x00000001u, 0x0004003bu, 0x0000000eu, 0x00000010u,
    0x00000003u, 0x0004003bu, 0x0000000au, 0x00000011u, 0x00000009u, 0x0004003bu,
    0x0000000cu, 0x00000012u, 0x00000000u, 0x0004002bu, 0x00000003u, 0x00000013u,
    0x00000000u, 0x0004002bu, 0x00000004u, 0x00000014u, 0x00000000u, 0x0004002bu,
    0x00000004u, 0x00000015u, 0x3f000000u, 0x0005002cu, 0x00000005u, 0x00000016u,
    0x00000015u, 0x00000015u, 0x00050036u, 0x00000001u, 0x00000017u, 0x00000000u,
    0x00000002u, 0x000200f8u, 0x00000018u, 0x0004003du, 0x00000006u, 0x00000019u,
    0x0000000fu, 0x0004003du, 0x00000008u, 0x0000001au, 0x00000012u, 0x00070058u,
    0x00000006u, 0x0000001bu, 0x0000001au, 0x00000016u, 0x00000002u, 0x00000014u,
    0x00050041u, 0x0000000bu, 0x0000001cu, 0x00000011u, 0x00000013u, 0x0004003du,
    0x00000006u, 0x0000001du, 0x0000001cu, 0x00050081u, 0x00000006u, 0x0000001eu,
    0x00000019u, 0x0000001bu, 0x00050081u, 0x00000006u, 0x0000001fu, 0x0000001eu,
    0x0000001du, 0x0003003eu, 0x00000010u, 0x0000001fu, 0x000100fdu, 0x00010038u,
};

static const char *pick_icd_path(int argc, char **argv)
{
    if (argc > 1 && argv[1][0]) return argv[1];
    const char *env = getenv("ARC_VULKAN_ICD");
    if (env && *env && access(env, R_OK) == 0) return env;
    if (access("./out/arcmtl/libvulkan_intel.dylib", R_OK) == 0)
        return "./out/arcmtl/libvulkan_intel.dylib";
    return "/Library/GPUBundles/arcmtl/libvulkan_intel.dylib";
}

int main(int argc, char **argv)
{
    const char *icd_path = pick_icd_path(argc, argv);

    printf("==================================================================\n");
    printf(" Stage 2: Mesa ANV Vulkan 1.3 Offscreen GPU Test (macOS 15+)\n");
    printf(" ICD: %s\n", icd_path);
    printf("==================================================================\n");

    void *lib = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "[FAIL] dlopen(%s) failed: %s\n", icd_path, dlerror());
        return 1;
    }

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) {
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    }
    if (!gipa) {
        fprintf(stderr, "[FAIL] Neither vk_icdGetInstanceProcAddr nor vkGetInstanceProcAddr found!\n");
        return 2;
    }

    PFN_vkCreateInstance pvkCreateInstance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!pvkCreateInstance) {
        fprintf(stderr, "[FAIL] Could not resolve vkCreateInstance\n");
        return 3;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "arc_vk_offscreen_test",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .pEngineName = "ArcXeTest",
        .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };

    VkInstance inst = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateInstance(&ici, NULL, &inst));

#define LOAD_INST(fn) PFN_##fn p##fn = (PFN_##fn)gipa(inst, #fn)
    LOAD_INST(vkDestroyInstance);
    LOAD_INST(vkEnumeratePhysicalDevices);
    LOAD_INST(vkGetPhysicalDeviceProperties);
    LOAD_INST(vkGetPhysicalDeviceFeatures2);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INST(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(vkCreateDevice);
    LOAD_INST(vkGetDeviceProcAddr);
#undef LOAD_INST

    uint32_t pdev_count = 0;
    CHECK_VK(pvkEnumeratePhysicalDevices(inst, &pdev_count, NULL));
    if (pdev_count == 0) {
        fprintf(stderr, "[FAIL] Mesa ANV enumerated 0 physical devices (check ArcXeService)!\n");
        return 4;
    }

    VkPhysicalDevice pdevs[8];
    if (pdev_count > 8) pdev_count = 8;
    CHECK_VK(pvkEnumeratePhysicalDevices(inst, &pdev_count, pdevs));

    VkPhysicalDevice pdev = pdevs[0];
    VkPhysicalDeviceProperties props;
    pvkGetPhysicalDeviceProperties(pdev, &props);

    printf("  VkPhysicalDevice   : %s\n", props.deviceName);
    printf("  Vendor / Device ID : %04x:%04x\n", props.vendorID, props.deviceID);
    printf("  Vulkan API Version : %u.%u.%u\n",
           VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion));

    VkPhysicalDeviceMemoryProperties memp;
    pvkGetPhysicalDeviceMemoryProperties(pdev, &memp);
    for (uint32_t i = 0; i < memp.memoryHeapCount; i++) {
        printf("  Memory Heap [%u]    : %" PRIu64 " MiB (flags=0x%x)\n",
               i, (uint64_t)memp.memoryHeaps[i].size / (1024ULL * 1024ULL),
               memp.memoryHeaps[i].flags);
    }

    uint32_t qf_count = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
    VkQueueFamilyProperties qfs[8];
    if (qf_count > 8) qf_count = 8;
    pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qfs);

    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            qfam = i;
            break;
        }
    }
    if (qfam == UINT32_MAX) {
        fprintf(stderr, "[FAIL] No graphics/compute queue family found!\n");
        return 5;
    }

    VkPhysicalDeviceBufferDeviceAddressFeatures bda_q = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
    };
    VkPhysicalDeviceFeatures2 feat2_q = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &bda_q,
    };
    if (pvkGetPhysicalDeviceFeatures2) {
        pvkGetPhysicalDeviceFeatures2(pdev, &feat2_q);
    }
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_en = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        .bufferDeviceAddress = bda_q.bufferDeviceAddress ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceFeatures en_feat = { 0 };
    en_feat.shaderInt64 = feat2_q.features.shaderInt64 ? VK_TRUE : VK_FALSE;

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = bda_en.bufferDeviceAddress ? &bda_en : NULL,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .pEnabledFeatures = &en_feat,
    };

    VkDevice dev = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateDevice(pdev, &dci, NULL, &dev));

#define LOAD_DEV(fn) PFN_##fn p##fn = (PFN_##fn)pvkGetDeviceProcAddr(dev, #fn)
    LOAD_DEV(vkDestroyDevice);
    LOAD_DEV(vkGetDeviceQueue);
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkGetBufferDeviceAddress);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);
    LOAD_DEV(vkCreateSampler);
    LOAD_DEV(vkDestroySampler);
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreatePipelineLayout);
    LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateGraphicsPipelines);
    LOAD_DEV(vkDestroyPipeline);
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreateQueryPool);
    LOAD_DEV(vkDestroyQueryPool);
    LOAD_DEV(vkGetQueryPoolResults);
    LOAD_DEV(vkCreateRenderPass);
    LOAD_DEV(vkDestroyRenderPass);
    LOAD_DEV(vkCreateFramebuffer);
    LOAD_DEV(vkDestroyFramebuffer);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkCmdCopyBuffer);
    LOAD_DEV(vkCmdFillBuffer);
    LOAD_DEV(vkCmdCopyBufferToImage);
    LOAD_DEV(vkCmdBeginRenderPass);
    LOAD_DEV(vkCmdEndRenderPass);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdPushConstants);
    LOAD_DEV(vkCmdSetViewport);
    LOAD_DEV(vkCmdSetScissor);
    LOAD_DEV(vkCmdDraw);
    LOAD_DEV(vkCmdResetQueryPool);
    LOAD_DEV(vkCmdWriteTimestamp);
    LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkCmdCopyImageToBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkCreateFence);
    LOAD_DEV(vkDestroyFence);
    LOAD_DEV(vkResetFences);
    LOAD_DEV(vkQueueSubmit);
    LOAD_DEV(vkWaitForFences);
#undef LOAD_DEV

    VkQueue queue = VK_NULL_HANDLE;
    pvkGetDeviceQueue(dev, qfam, 0, &queue);

    const VkDeviceSize buf_size = 65536;
    VkBufferCreateInfo src_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buf_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBufferCreateInfo dst_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buf_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer src_buf = VK_NULL_HANDLE;
    VkBuffer buf     = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateBuffer(dev, &src_bci, NULL, &src_buf));
    CHECK_VK(pvkCreateBuffer(dev, &dst_bci, NULL, &buf));

    VkMemoryRequirements src_mreq, mreq;
    pvkGetBufferMemoryRequirements(dev, src_buf, &src_mreq);
    pvkGetBufferMemoryRequirements(dev, buf, &mreq);

    const VkDeviceSize align = (mreq.alignment > 4096) ? mreq.alignment : 4096;
    const VkDeviceSize dst_offset = (src_mreq.size + align - 1ULL) & ~(align - 1ULL);
    const VkDeviceSize total_alloc = dst_offset + mreq.size;

    uint32_t mem_idx = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t common_bits = src_mreq.memoryTypeBits & mreq.memoryTypeBits;
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
        if ((common_bits & (1u << i)) &&
            (memp.memoryTypes[i].propertyFlags & want) == want) {
            mem_idx = i;
            break;
        }
    }
    if (mem_idx == UINT32_MAX) {
        fprintf(stderr, "[FAIL] No HOST_VISIBLE | HOST_COHERENT UMA memory type found!\n");
        return 6;
    }
    printf("  Selected MemType   : [%u] (flags=0x%x)\n",
           mem_idx, memp.memoryTypes[mem_idx].propertyFlags);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = total_alloc,
        .memoryTypeIndex = mem_idx,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK_VK(pvkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK_VK(pvkBindBufferMemory(dev, src_buf, mem, 0));
    CHECK_VK(pvkBindBufferMemory(dev, buf, mem, dst_offset));

    void *mapped_all = NULL;
    CHECK_VK(pvkMapMemory(dev, mem, 0, total_alloc, 0, &mapped_all));
    uint32_t *src_words = (uint32_t *)mapped_all;
    volatile uint32_t *words = (volatile uint32_t *)((uint8_t *)mapped_all + dst_offset);
    const uint32_t total_words = (uint32_t)(buf_size / sizeof(uint32_t));

    const uint32_t copy_pattern = 0xA5C07D55u;
    for (uint32_t i = 0; i < total_words; i++) {
        src_words[i] = copy_pattern;
        words[i]     = 0xDEADBEEFu; /* Non-zero sentinel distinguishes untouched vs zero-written */
    }

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateCommandPool(dev, &cpci, NULL, &pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 6,
    };
    VkCommandBuffer cmds[6] = { VK_NULL_HANDLE };
    CHECK_VK(pvkAllocateCommandBuffers(dev, &cbai, cmds));

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = 0,
    };
    VkFence fence = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateFence(dev, &fci, NULL, &fence));

    /* Subtest B1: vkCmdCopyBuffer (64 KiB) */
    VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = buf_size };
    CHECK_VK(pvkBeginCommandBuffer(cmds[0], &cbbi));
    pvkCmdCopyBuffer(cmds[0], src_buf, buf, 1, &region);
    CHECK_VK(pvkEndCommandBuffer(cmds[0]));

    VkSubmitInfo si0 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmds[0],
    };
    printf("\n  [B1/4] Submitting vkCmdCopyBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
           copy_pattern, qfam);
    CHECK_VK(pvkQueueSubmit(queue, 1, &si0, fence));
    CHECK_VK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL));

    uint32_t copy_mismatches = 0;
    for (uint32_t i = 0; i < total_words; i++) {
        if (words[i] != copy_pattern) {
            if (copy_mismatches < 4) {
                fprintf(stderr, "  [MISMATCH B1] word[%u]: expected 0x%08x, got 0x%08x\n",
                        i, copy_pattern, words[i]);
            }
            copy_mismatches++;
        }
    }
    if (copy_mismatches == 0) {
        printf("  [PASS B1] Verified %u / %u uint32 words == 0x%08x!\n",
               total_words, total_words, copy_pattern);
    }

    /* Subtest B2: vkCmdFillBuffer (64 KiB 3D/Compute BLORP) */
    for (uint32_t i = 0; i < total_words; i++) {
        words[i] = 0xCAFEBABEu;
    }
    CHECK_VK(pvkResetFences(dev, 1, &fence));

    const uint32_t pattern = 0x155A7D55u;
    CHECK_VK(pvkBeginCommandBuffer(cmds[1], &cbbi));
    pvkCmdFillBuffer(cmds[1], buf, 0, buf_size, pattern);
    CHECK_VK(pvkEndCommandBuffer(cmds[1]));

    VkSubmitInfo si1 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmds[1],
    };
    printf("  [B2/4] Submitting vkCmdFillBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
           pattern, qfam);
    CHECK_VK(pvkQueueSubmit(queue, 1, &si1, fence));
    CHECK_VK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL));

    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < total_words; i++) {
        if (words[i] != pattern) {
            if (mismatches < 4) {
                fprintf(stderr, "  [MISMATCH B2] word[%u]: expected 0x%08x, got 0x%08x\n",
                        i, pattern, words[i]);
            }
            mismatches++;
        }
    }
    if (mismatches == 0) {
        printf("  [PASS B2] Verified %u / %u uint32 words == 0x%08x!\n",
               total_words, total_words, pattern);
    }

    /*
     * Subtest B3: Optimal-Tiled 3D VkImage RenderPass Clear + vkCmdCopyImageToBuffer Readback
     * Exercises the exact 3D Color Attachment -> BLORP Image-to-Buffer readback pipeline used
     * by WindowServer / ArcMTLDriver.bundle on every desktop frame!
     */
    const uint32_t img_w = 64, img_h = 64;
    const uint32_t img_pixels = img_w * img_h;
    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { img_w, img_h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateImage(dev, &img_ci, NULL, &img));

    VkMemoryRequirements img_mreq;
    pvkGetImageMemoryRequirements(dev, img, &img_mreq);
    uint32_t img_mem_idx = UINT32_MAX;
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
        if ((img_mreq.memoryTypeBits & (1u << i)) &&
            (memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            img_mem_idx = i;
            break;
        }
    }
    if (img_mem_idx == UINT32_MAX) {
        for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
            if (img_mreq.memoryTypeBits & (1u << i)) {
                img_mem_idx = i;
                break;
            }
        }
    }
    VkMemoryAllocateInfo img_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = img_mreq.size,
        .memoryTypeIndex = img_mem_idx,
    };
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    CHECK_VK(pvkAllocateMemory(dev, &img_mai, NULL, &img_mem));
    CHECK_VK(pvkBindImageMemory(dev, img, img_mem, 0));

    VkImageViewCreateInfo iv_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView iv = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateImageView(dev, &iv_ci, NULL, &iv));

    VkAttachmentDescription att = {
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateRenderPass(dev, &rp_ci, NULL, &rp));

    VkFramebufferCreateInfo fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1,
        .pAttachments = &iv,
        .width = img_w,
        .height = img_h,
        .layers = 1,
    };
    VkFramebuffer fb = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateFramebuffer(dev, &fb_ci, NULL, &fb));

    for (uint32_t i = 0; i < img_pixels; i++) {
        words[i] = 0x11223344u;
    }
    CHECK_VK(pvkResetFences(dev, 1, &fence));

    VkClearValue clear_val = {
        .color = { .float32 = { 64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f } }
    };
    VkRenderPassBeginInfo rp_bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp,
        .framebuffer = fb,
        .renderArea = { { 0, 0 }, { img_w, img_h } },
        .clearValueCount = 1,
        .pClearValues = &clear_val,
    };
    VkImageMemoryBarrier img_bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { img_w, img_h, 1 },
    };

    CHECK_VK(pvkBeginCommandBuffer(cmds[2], &cbbi));
    pvkCmdBeginRenderPass(cmds[2], &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdEndRenderPass(cmds[2]);
    pvkCmdPipelineBarrier(cmds[2],
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &img_bar);
    pvkCmdCopyImageToBuffer(cmds[2], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    CHECK_VK(pvkEndCommandBuffer(cmds[2]));

    VkSubmitInfo si2 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmds[2],
    };
    printf("  [B3/4] Submitting Optimal-Tiled 3D VkImage RenderPass + vkCmdCopyImageToBuffer (%ux%u BGRA8)...\n",
           img_w, img_h);
    CHECK_VK(pvkQueueSubmit(queue, 1, &si2, fence));
    CHECK_VK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL));

    uint32_t img_mismatches = 0;
    for (uint32_t i = 0; i < img_pixels; i++) {
        const uint32_t px = words[i];
        const int b = (int)(px & 0xFFu);
        const int g = (int)((px >> 8) & 0xFFu);
        const int r = (int)((px >> 16) & 0xFFu);
        const int a = (int)((px >> 24) & 0xFFu);
        if (abs(b - 0xC0) > 1 || abs(g - 0x80) > 1 || abs(r - 0x40) > 1 || a != 0xFF) {
            if (img_mismatches < 4) {
                fprintf(stderr, "  [MISMATCH B3] pixel[%u]: expected ~0xFF4080C0, got 0x%08x\n", i, px);
            }
            img_mismatches++;
        }
    }
    if (img_mismatches == 0) {
        printf("  [PASS B3] Verified %u / %u rendered BGRA8 pixels == 0x%08x!\n",
               img_pixels, img_pixels, words[0]);
    }

    /*
     * Subtest B4: Multi-Batch Full 3D Graphics Pipeline (WindowServer CoreDisplay Reproducer)
     * Exercises 3 consecutive 3D draw batches using:
     *   - VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC (3DSTATE_CONSTANT_VS, requires CS_DEBUG_MODE2 BIT(4))
     *   - PhysicalStorageBuffer64 64-bit BDA pointer dereference in VS
     *   - vkCmdPushConstants in FS (3DSTATE_CONSTANT_PS)
     *   - Optimal-tiled sampled BGRA8 texture (VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
     *   -VkQueryPool hardware timestamps + Optimal-tiled BGRA8 render target readback
     */
    uint32_t b4_mismatches = 0;
    if (!bda_en.bufferDeviceAddress || !en_feat.shaderInt64 || !pvkGetBufferDeviceAddress) {
        fprintf(stderr, "  [FAIL B4] bufferDeviceAddress (%u) or shaderInt64 (%u) unavailable!\n",
                (unsigned)bda_en.bufferDeviceAddress, (unsigned)en_feat.shaderInt64);
        b4_mismatches = 1;
    } else {
        VkBufferCreateInfo dyn_bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBufferCreateInfo bda_bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer dyn_buf = VK_NULL_HANDLE, bda_buf = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateBuffer(dev, &dyn_bci, NULL, &dyn_buf));
        CHECK_VK(pvkCreateBuffer(dev, &bda_bci, NULL, &bda_buf));

        VkMemoryRequirements dyn_mr, bda_mr;
        pvkGetBufferMemoryRequirements(dev, dyn_buf, &dyn_mr);
        pvkGetBufferMemoryRequirements(dev, bda_buf, &bda_mr);
        const VkDeviceSize b4_align = (bda_mr.alignment > 4096) ? bda_mr.alignment : 4096;
        const VkDeviceSize bda_off  = (dyn_mr.size + b4_align - 1ULL) & ~(b4_align - 1ULL);
        const VkDeviceSize b4_total = bda_off + bda_mr.size;

        uint32_t b4_mem_idx = UINT32_MAX;
        const uint32_t b4_bits = dyn_mr.memoryTypeBits & bda_mr.memoryTypeBits;
        for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
            if ((b4_bits & (1u << i)) &&
                (memp.memoryTypes[i].propertyFlags & want) == want) {
                b4_mem_idx = i;
                break;
            }
        }
        if (b4_mem_idx == UINT32_MAX) {
            fprintf(stderr, "  [FAIL B4] No coherent UMA memory type for BDA/UBO!\n");
            return 14;
        }
        VkMemoryAllocateFlagsInfo b4_mafi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        VkMemoryAllocateInfo b4_mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &b4_mafi,
            .allocationSize = b4_total,
            .memoryTypeIndex = b4_mem_idx,
        };
        VkDeviceMemory b4_mem = VK_NULL_HANDLE;
        CHECK_VK(pvkAllocateMemory(dev, &b4_mai, NULL, &b4_mem));
        CHECK_VK(pvkBindBufferMemory(dev, dyn_buf, b4_mem, 0));
        CHECK_VK(pvkBindBufferMemory(dev, bda_buf, b4_mem, bda_off));

        void *b4_map = NULL;
        CHECK_VK(pvkMapMemory(dev, b4_mem, 0, b4_total, 0, &b4_map));
        memset(b4_map, 0, (size_t)b4_total);

        VkBufferDeviceAddressInfo bdai = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = bda_buf,
        };
        const VkDeviceAddress bda_base = pvkGetBufferDeviceAddress(dev, &bdai);
        uint8_t *dyn_bytes = (uint8_t *)b4_map;
        uint8_t *bda_bytes = (uint8_t *)b4_map + bda_off;

        /* 3 dynamic UBO entries at 256-byte intervals pointing to 3 distinct 64-bit BDA payloads */
        const float bda_colors[3][4] = {
            {  32.0f / 255.0f,  16.0f / 255.0f,  64.0f / 255.0f, 127.0f / 255.0f },
            {  96.0f / 255.0f,  32.0f / 255.0f,  96.0f / 255.0f, 127.0f / 255.0f },
            { 160.0f / 255.0f,  80.0f / 255.0f, 128.0f / 255.0f, 127.0f / 255.0f },
        };
        const float pc_colors[3][4] = {
            {  16.0f / 255.0f,  20.0f / 255.0f,  24.0f / 255.0f,   0.0f },
            {  24.0f / 255.0f,  44.0f / 255.0f,  32.0f / 255.0f,   0.0f },
            {  32.0f / 255.0f,  36.0f / 255.0f,  40.0f / 255.0f,   0.0f },
        };
        /* Sampled 4x4 BGRA8 texel = B=32, G=64, R=16, A=128 -> shader RGBA=(16, 64, 32, 128)/255 */
        const int exp_rgba[3][4] = {
            {  64, 100, 120, 255 },
            { 136, 140, 160, 255 },
            { 208, 180, 200, 255 },
        };
        for (uint32_t b = 0; b < 3; b++) {
            const uint64_t ptr_val = (uint64_t)bda_base + (uint64_t)(b * 64u);
            memcpy(dyn_bytes + b * 256u, &ptr_val, sizeof(ptr_val));
            memcpy(bda_bytes + b * 64u, bda_colors[b], sizeof(bda_colors[b]));
        }
        uint32_t *tex_staging = (uint32_t *)(bda_bytes + 256u);
        for (uint32_t i = 0; i < 16; i++) {
            tex_staging[i] = 0x80104020u; /* BGRA8: B=0x20(32), G=0x40(64), R=0x10(16), A=0x80(128) */
        }

        /* 4x4 Optimal-Tiled Sampled Texture + Sampler */
        VkImageCreateInfo tex_ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .extent = { 4, 4, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage tex_img = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateImage(dev, &tex_ci, NULL, &tex_img));
        VkMemoryRequirements tex_mr;
        pvkGetImageMemoryRequirements(dev, tex_img, &tex_mr);
        uint32_t tex_mem_idx = img_mem_idx;
        if (!(tex_mr.memoryTypeBits & (1u << tex_mem_idx))) {
            for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
                if (tex_mr.memoryTypeBits & (1u << i)) { tex_mem_idx = i; break; }
            }
        }
        VkMemoryAllocateInfo tex_mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = tex_mr.size,
            .memoryTypeIndex = tex_mem_idx,
        };
        VkDeviceMemory tex_mem = VK_NULL_HANDLE;
        CHECK_VK(pvkAllocateMemory(dev, &tex_mai, NULL, &tex_mem));
        CHECK_VK(pvkBindImageMemory(dev, tex_img, tex_mem, 0));

        VkImageViewCreateInfo tex_ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = tex_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkImageView tex_iv = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateImageView(dev, &tex_ivci, NULL, &tex_iv));

        VkSamplerCreateInfo samp_ci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 1.0f,
        };
        VkSampler b4_samp = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateSampler(dev, &samp_ci, NULL, &b4_samp));

        /* Shaders, Descriptor Set Layout, Pipeline Layout & Graphics Pipeline */
        VkShaderModuleCreateInfo vs_ci = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(k_b4_vs_spv),
            .pCode = k_b4_vs_spv,
        };
        VkShaderModuleCreateInfo fs_ci = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(k_b4_fs_spv),
            .pCode = k_b4_fs_spv,
        };
        VkShaderModule b4_vs = VK_NULL_HANDLE, b4_fs = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateShaderModule(dev, &vs_ci, NULL, &b4_vs));
        CHECK_VK(pvkCreateShaderModule(dev, &fs_ci, NULL, &b4_fs));

        VkDescriptorSetLayoutBinding dsl_bindings[2] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        };
        VkDescriptorSetLayoutCreateInfo dsl_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = dsl_bindings,
        };
        VkDescriptorSetLayout b4_dsl = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateDescriptorSetLayout(dev, &dsl_ci, NULL, &b4_dsl));

        VkPushConstantRange pc_range = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 16,
        };
        VkPipelineLayoutCreateInfo pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &b4_dsl,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pc_range,
        };
        VkPipelineLayout b4_pl = VK_NULL_HANDLE;
        CHECK_VK(pvkCreatePipelineLayout(dev, &pl_ci, NULL, &b4_pl));

        VkPipelineShaderStageCreateInfo stages[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = b4_vs,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = b4_fs,
                .pName = "main",
            },
        };
        VkPipelineVertexInputStateCreateInfo vi_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo ia_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo vp_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState cba_st = {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo cb_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &cba_st,
        };
        VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn_st = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dyn_states,
        };
        VkGraphicsPipelineCreateInfo gp_ci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vi_st,
            .pInputAssemblyState = &ia_st,
            .pViewportState = &vp_st,
            .pRasterizationState = &rs_st,
            .pMultisampleState = &ms_st,
            .pColorBlendState = &cb_st,
            .pDynamicState = &dyn_st,
            .layout = b4_pl,
            .renderPass = rp,
            .subpass = 0,
        };
        VkPipeline b4_pipe = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp_ci, NULL, &b4_pipe));

        VkDescriptorPoolSize pool_sizes[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
        };
        VkDescriptorPoolCreateInfo dp_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 2,
            .pPoolSizes = pool_sizes,
        };
        VkDescriptorPool b4_dpool = VK_NULL_HANDLE;
        CHECK_VK(pvkCreateDescriptorPool(dev, &dp_ci, NULL, &b4_dpool));

        VkDescriptorSetAllocateInfo ds_ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = b4_dpool,
            .descriptorSetCount = 1,
            .pSetLayouts = &b4_dsl,
        };
        VkDescriptorSet b4_dset = VK_NULL_HANDLE;
        CHECK_VK(pvkAllocateDescriptorSets(dev, &ds_ai, &b4_dset));

        VkDescriptorBufferInfo dbi = {
            .buffer = dyn_buf,
            .offset = 0,
            .range = 16,
        };
        VkDescriptorImageInfo dii = {
            .sampler = b4_samp,
            .imageView = tex_iv,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = b4_dset,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .pBufferInfo = &dbi,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = b4_dset,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &dii,
            },
        };
        pvkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

        VkQueryPool b4_qp = VK_NULL_HANDLE;
        if (qfs[qfam].timestampValidBits > 0 && pvkCreateQueryPool) {
            VkQueryPoolCreateInfo qp_ci = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 6,
            };
            CHECK_VK(pvkCreateQueryPool(dev, &qp_ci, NULL, &b4_qp));
        }

        printf("  [B4/4] Submitting 3 consecutive 3D Draw batches (UNIFORM_BUFFER_DYNAMIC + 64-bit BDA=0x%" PRIx64 " + PushConst + Sampled OPTIMAL)...\n",
               (uint64_t)bda_base);

        for (uint32_t b = 0; b < 3; b++) {
            for (uint32_t i = 0; i < img_pixels; i++) {
                words[i] = 0x55AA7733u;
            }
            CHECK_VK(pvkResetFences(dev, 1, &fence));

            VkCommandBuffer cmd = cmds[3 + b];
            CHECK_VK(pvkBeginCommandBuffer(cmd, &cbbi));

            if (b4_qp) {
                pvkCmdResetQueryPool(cmd, b4_qp, b * 2u, 2u);
                pvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b4_qp, b * 2u);
            }

            if (b == 0) {
                VkImageMemoryBarrier to_dst = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = tex_img,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                };
                pvkCmdPipelineBarrier(cmd,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0, 0, NULL, 0, NULL, 1, &to_dst);
                VkBufferImageCopy tex_bic = {
                    .bufferOffset = 256,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageOffset = { 0, 0, 0 },
                    .imageExtent = { 4, 4, 1 },
                };
                pvkCmdCopyBufferToImage(cmd, bda_buf, tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tex_bic);
                VkImageMemoryBarrier to_sample = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = tex_img,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                };
                pvkCmdPipelineBarrier(cmd,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      0, 0, NULL, 0, NULL, 1, &to_sample);
            }

            VkClearValue b4_clear = {
                .color = { .float32 = { 4.0f / 255.0f, 8.0f / 255.0f, 12.0f / 255.0f, 1.0f } }
            };
            VkRenderPassBeginInfo b4_rpbi = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = rp,
                .framebuffer = fb,
                .renderArea = { { 0, 0 }, { img_w, img_h } },
                .clearValueCount = 1,
                .pClearValues = &b4_clear,
            };
            pvkCmdBeginRenderPass(cmd, &b4_rpbi, VK_SUBPASS_CONTENTS_INLINE);
            pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b4_pipe);

            VkViewport vp = { 0.0f, 0.0f, (float)img_w, (float)img_h, 0.0f, 1.0f };
            VkRect2D sc   = { { 0, 0 }, { img_w, img_h } };
            pvkCmdSetViewport(cmd, 0, 1, &vp);
            pvkCmdSetScissor(cmd, 0, 1, &sc);

            const uint32_t dyn_off = b * 256u;
            pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b4_pl,
                                     0, 1, &b4_dset, 1, &dyn_off);
            pvkCmdPushConstants(cmd, b4_pl, VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(pc_colors[b]), pc_colors[b]);
            pvkCmdDraw(cmd, 3, 1, 0, 0);
            pvkCmdEndRenderPass(cmd);

            pvkCmdPipelineBarrier(cmd,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, NULL, 0, NULL, 1, &img_bar);
            pvkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);

            if (b4_qp) {
                pvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b4_qp, b * 2u + 1u);
            }
            CHECK_VK(pvkEndCommandBuffer(cmd));

            VkSubmitInfo b4_si = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd,
            };
            CHECK_VK(pvkQueueSubmit(queue, 1, &b4_si, fence));
            CHECK_VK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL));

            uint64_t ts_pair[2] = { 0, 0 };
            if (b4_qp) {
                CHECK_VK(pvkGetQueryPoolResults(dev, b4_qp, b * 2u, 2u,
                                                sizeof(ts_pair), ts_pair, sizeof(uint64_t),
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            }

            uint32_t batch_bad = 0;
            for (uint32_t i = 0; i < img_pixels; i++) {
                const uint32_t px = words[i];
                const int pb = (int)(px & 0xFFu);
                const int pg = (int)((px >> 8) & 0xFFu);
                const int pr = (int)((px >> 16) & 0xFFu);
                const int pa = (int)((px >> 24) & 0xFFu);
                if (abs(pr - exp_rgba[b][0]) > 2 ||
                    abs(pg - exp_rgba[b][1]) > 2 ||
                    abs(pb - exp_rgba[b][2]) > 2 ||
                    abs(pa - exp_rgba[b][3]) > 2) {
                    if (batch_bad < 2) {
                        fprintf(stderr,
                                "  [MISMATCH B4 batch #%u] pixel[%u]: expected RGBA=(%d,%d,%d,%d), got BGRA=0x%08x\n",
                                b + 1u, i, exp_rgba[b][0], exp_rgba[b][1], exp_rgba[b][2], exp_rgba[b][3], px);
                    }
                    batch_bad++;
                }
            }
            b4_mismatches += batch_bad;
            if (batch_bad == 0) {
                printf("  [PASS B4.%u] Batch #%u 3D Draw verified (%u px == 0x%08x, dyn_off=%u, gpu_ts=%" PRIu64 "..%" PRIu64 ")\n",
                       b + 1u, b + 1u, img_pixels, words[0], dyn_off, ts_pair[0], ts_pair[1]);
            }
        }

        if (b4_qp) pvkDestroyQueryPool(dev, b4_qp, NULL);
        pvkDestroyDescriptorPool(dev, b4_dpool, NULL);
        pvkDestroyPipeline(dev, b4_pipe, NULL);
        pvkDestroyPipelineLayout(dev, b4_pl, NULL);
        pvkDestroyDescriptorSetLayout(dev, b4_dsl, NULL);
        pvkDestroyShaderModule(dev, b4_fs, NULL);
        pvkDestroyShaderModule(dev, b4_vs, NULL);
        pvkDestroySampler(dev, b4_samp, NULL);
        pvkDestroyImageView(dev, tex_iv, NULL);
        pvkDestroyImage(dev, tex_img, NULL);
        pvkFreeMemory(dev, tex_mem, NULL);
        pvkUnmapMemory(dev, b4_mem);
        pvkDestroyBuffer(dev, bda_buf, NULL);
        pvkDestroyBuffer(dev, dyn_buf, NULL);
        pvkFreeMemory(dev, b4_mem, NULL);
    }

    pvkDestroyFramebuffer(dev, fb, NULL);
    pvkDestroyRenderPass(dev, rp, NULL);
    pvkDestroyImageView(dev, iv, NULL);
    pvkDestroyImage(dev, img, NULL);
    pvkFreeMemory(dev, img_mem, NULL);

    int rc = 0;
    if (copy_mismatches == 0 && mismatches == 0 && img_mismatches == 0 && b4_mismatches == 0) {
        printf("\n[PASS] Stage 2 PASSED: Mesa ANV Vulkan 1.3 Buffer + Optimal-Tiled 3D Draw + Dynamic UBO/BDA executed cleanly on Intel Arc!\n");
    } else {
        fprintf(stderr, "\n[FAIL] Stage 2 FAILED: B1=%u, B2=%u, B3_img=%u, B4_3d=%u mismatches!\n",
                copy_mismatches, mismatches, img_mismatches, b4_mismatches);
        rc = 12;
    }

    pvkDestroyFence(dev, fence, NULL);
    pvkDestroyCommandPool(dev, pool, NULL);
    pvkUnmapMemory(dev, mem);
    pvkDestroyBuffer(dev, buf, NULL);
    pvkDestroyBuffer(dev, src_buf, NULL);
    pvkFreeMemory(dev, mem, NULL);
    pvkDestroyDevice(dev, NULL);
    pvkDestroyInstance(inst, NULL);
    dlclose(lib);
    return rc;
}
