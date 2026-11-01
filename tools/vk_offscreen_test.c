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

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };

    VkDevice dev = VK_NULL_HANDLE;
    CHECK_VK(pvkCreateDevice(pdev, &dci, NULL, &dev));

#define LOAD_DEV(fn) PFN_##fn p##fn = (PFN_##fn)pvkGetDeviceProcAddr(dev, #fn)
    LOAD_DEV(vkDestroyDevice);
    LOAD_DEV(vkGetDeviceQueue);
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkCmdCopyBuffer);
    LOAD_DEV(vkCmdFillBuffer);
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
        .commandBufferCount = 2,
    };
    VkCommandBuffer cmds[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
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
    printf("\n  [B1/2] Submitting vkCmdCopyBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
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
    printf("  [B2/2] Submitting vkCmdFillBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
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

    int rc = 0;
    if (copy_mismatches == 0 && mismatches == 0) {
        printf("\n[PASS] Stage 2 PASSED: Mesa ANV Vulkan 1.3 Copy + Fill executed cleanly on Intel Arc!\n");
    } else {
        fprintf(stderr, "\n[FAIL] Stage 2 FAILED: B1_mismatches=%u, B2_mismatches=%u (of %u words)!\n",
                copy_mismatches, mismatches, total_words);
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
