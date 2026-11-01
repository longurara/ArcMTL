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
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);
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
    LOAD_DEV(vkCmdBeginRenderPass);
    LOAD_DEV(vkCmdEndRenderPass);
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
        .commandBufferCount = 3,
    };
    VkCommandBuffer cmds[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
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
    printf("\n  [B1/3] Submitting vkCmdCopyBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
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
    printf("  [B2/3] Submitting vkCmdFillBuffer (64 KiB, pattern=0x%08x) on queue family %u...\n",
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
    printf("  [B3/3] Submitting Optimal-Tiled 3D VkImage RenderPass + vkCmdCopyImageToBuffer (%ux%u BGRA8)...\n",
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

    pvkDestroyFramebuffer(dev, fb, NULL);
    pvkDestroyRenderPass(dev, rp, NULL);
    pvkDestroyImageView(dev, iv, NULL);
    pvkDestroyImage(dev, img, NULL);
    pvkFreeMemory(dev, img_mem, NULL);

    int rc = 0;
    if (copy_mismatches == 0 && mismatches == 0 && img_mismatches == 0) {
        printf("\n[PASS] Stage 2 PASSED: Mesa ANV Vulkan 1.3 Buffer + Optimal-Tiled 3D VkImage executed cleanly on Intel Arc!\n");
    } else {
        fprintf(stderr, "\n[FAIL] Stage 2 FAILED: B1=%u, B2=%u, B3_img=%u mismatches!\n",
                copy_mismatches, mismatches, img_mismatches);
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
