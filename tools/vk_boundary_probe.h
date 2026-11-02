/* Diagnostic-only queue re-entry probe. Uses existing coherent B1 buffers. */
typedef struct ArcVkBoundaryProbe {
    VkDevice dev;
    VkQueue queue;
    VkFence fence;
    VkBuffer src, dst;
    VkDeviceSize offset;
    uint32_t *src_word;
    volatile uint32_t *dst_word;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkCmdCopyBuffer copy;
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkEndCommandBuffer end;
    PFN_vkResetFences reset;
    PFN_vkQueueSubmit submit;
    PFN_vkWaitForFences wait;
} ArcVkBoundaryProbe;

static int arc_vk_boundary_probe(const ArcVkBoundaryProbe *p,
                                 VkCommandBuffer cmd, const char *label,
                                 uint32_t expected)
{
    printf("  [BOUNDARY %s] BEGIN copy/re-entry, expected=0x%08x\n", label, expected);
    fflush(stdout);
    *p->src_word = expected;
    *p->dst_word = ~expected;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkBufferCopy region = { p->offset, p->offset, sizeof(uint32_t) };
    VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = p->dst,
        .offset = p->offset,
        .size = sizeof(uint32_t),
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    const char *step = "begin";
    VkResult result = p->begin(cmd, &bi);
    if (result != VK_SUCCESS) goto fail;
    p->copy(cmd, p->src, p->dst, 1, &region);
    p->barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
               0, 0, NULL, 1, &host_read, 0, NULL);
    step = "end";
    result = p->end(cmd);
    if (result != VK_SUCCESS) goto fail;
    step = "reset-fence";
    result = p->reset(p->dev, 1, &p->fence);
    if (result != VK_SUCCESS) goto fail;
    step = "submit";
    result = p->submit(p->queue, 1, &si, p->fence);
    if (result != VK_SUCCESS) goto fail;
    printf("  [BOUNDARY %s] SUBMITTED; waiting for fence\n", label);
    fflush(stdout);
    step = "wait";
    result = p->wait(p->dev, 1, &p->fence, VK_TRUE, 3000000000ULL);
    if (result != VK_SUCCESS) goto fail;
    const uint32_t actual = *p->dst_word;
    if (actual != expected) {
        fprintf(stderr, "  [BOUNDARY %s] FAIL readback expected=0x%08x actual=0x%08x\n",
                label, expected, actual);
        return 1;
    }
    printf("  [BOUNDARY %s] PASS copy + fence + readback=0x%08x\n", label, actual);
    fflush(stdout);
    return 0;
fail:
    fprintf(stderr, "  [BOUNDARY %s] FAIL %s VkResult=%d\n", label, step, (int)result);
    return 1;
}
