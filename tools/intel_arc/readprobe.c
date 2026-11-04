// GPU read speed from driver-allocated memory (each memory type), a memfd host import (4 KiB
// pages unless shmem THP is on) and an anonymous THP host import, 1 GiB each, timed with
// timestamps around a compute shader: read.comp sequential, gather.comp scattered (-DGATHER).
// Build: glslangValidator -V --target-env vulkan1.2 -o read.spv read.comp && xxd -i read.spv > read_spv.h
//        glslangValidator -V --target-env vulkan1.2 -o gather.spv gather.comp &&
//          xxd -i gather.spv | sed 's/gather_spv/read_spv/g' > gather_spv.h
//        gcc -O2 -o readprobe readprobe.c -lvulkan; gcc -O2 -DGATHER -o gatherprobe readprobe.c -lvulkan
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifdef GATHER
#include "gather_spv.h"
#else
#include "read_spv.h"
#endif

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s: %d\n", #x, r_); exit(1); } } while (0)
static const VkDeviceSize SIZE = 1024ull << 20;
static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue queue;
static uint32_t qfam;
static float ts_period;

static VkBuffer make_buffer(VkDeviceSize size, int external) {
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &ext : NULL, 0, size,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
    return b;
}

static double run(VkBuffer src, const char* what) {
    static VkPipeline pipe;
    static VkPipelineLayout layout;
    static VkDescriptorSetLayout dsl;
    static VkBuffer dst;
    static VkCommandPool pool;
    static VkQueryPool qp;
    static PFN_vkCmdPushDescriptorSetKHR push;
    if (!pipe) {
        VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
                                               VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 2, b};
        CHECK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl));
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pc};
        CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
        VkShaderModuleCreateInfo sci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, read_spv_len,
                                        (const uint32_t*)read_spv};
        VkShaderModule m;
        CHECK(vkCreateShaderModule(dev, &sci, NULL, &m));
        VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                            VK_SHADER_STAGE_COMPUTE_BIT, m, "main"},
                                           layout};
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
        dst = make_buffer(4u << 20, 0);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, dst, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                   (uint32_t)__builtin_ctz(req.memoryTypeBits)};
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
        CHECK(vkBindBufferMemory(dev, dst, mem, 0));
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
        CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
        CHECK(vkCreateQueryPool(dev, &qci, NULL, &qp));
        push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    }
    double best = 1e30;
    for (int rep = 0; rep < 6; rep++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cb, &bi));
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        VkDescriptorBufferInfo in[2] = {{src, 0, SIZE}, {dst, 0, 4u << 20}};
        VkWriteDescriptorSet w[2] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &in[0]},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &in[1]}};
        push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, w);
        uint32_t groups = 4096, threads = groups * 256, count = (uint32_t)(SIZE / 16);
        #ifdef GATHER
        uint32_t pcv[2] = {count, 16};
#else
        uint32_t pcv[2] = {count, (count + threads - 1) / threads};
#endif
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        vkCmdDispatch(cb, groups, 1, 1);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        uint64_t t[2];
        CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double ms = (t[1] - t[0]) * ts_period / 1e6;
        if (rep > 0 && ms < best) best = ms;  // the first run warms caches and clocks
        vkFreeCommandBuffers(dev, pool, 1, &cb);
    }
    #ifdef GATHER
    printf("%-44s %7.3f ms  %6.1f GB/s scattered\n", what, best, 4096.0 * 256 * 16 * 16 / best / 1e6);
#else
    printf("%-44s %7.3f ms  %6.1f GB/s\n", what, best, SIZE / best / 1e6);
#endif
    return best;
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(pds[i], &pr);
        if (pr.vendorID == 0x8086) { pd = pds[i]; ts_period = pr.limits.timestampPeriod; }
    }
    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char* ext[] = {"VK_EXT_external_memory_host", "VK_KHR_push_descriptor"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &q, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBuffer probe = make_buffer(SIZE, 0);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, probe, &req);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if (!(req.memoryTypeBits & (1u << t))) continue;
        VkBuffer b = make_buffer(SIZE, 0);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
        CHECK(vkBindBufferMemory(dev, b, mem, 0));
        VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            void* p;
            CHECK(vkMapMemory(dev, mem, 0, SIZE, 0, &p));
            memset(p, 1, SIZE);
            vkUnmapMemory(dev, mem);
        }
        char what[96];
        snprintf(what, sizeof what, "driver type %u (%s%s%s)", t, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DL " : "",
                 f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HV " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "HC" : "");
        run(b, what);
    }
    PFN_vkGetMemoryHostPointerPropertiesEXT get_props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    int fd = memfd_create("readprobe", 0);
    if (ftruncate(fd, SIZE)) return 1;
    char* host = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(host, 1, SIZE);
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(get_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp));
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if (!(hp.memoryTypeBits & (1u << t))) continue;
        VkBuffer b = make_buffer(SIZE, 1);
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, SIZE, t};
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
        CHECK(vkBindBufferMemory(dev, b, mem, 0));
        char what[96];
        snprintf(what, sizeof what, "memfd host import into type %u", t);
        run(b, what);
    }
    {
        char* anon = mmap(NULL, SIZE + (2u << 20), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        anon = (char*)(((uintptr_t)anon + (2u << 20) - 1) & ~(uintptr_t)((2u << 20) - 1));
        madvise(anon, SIZE, MADV_HUGEPAGE);
        memset(anon, 1, SIZE);
        VkBuffer b = make_buffer(SIZE, 1);
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, anon};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, SIZE, 1};
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
        CHECK(vkBindBufferMemory(dev, b, mem, 0));
        FILE* f = fopen("/proc/self/smaps_rollup", "r");
        char line[256]; long thp = 0;
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "AnonHugePages:", 14)) thp = atol(line + 14);
        if (f) fclose(f);
        char what[96];
        snprintf(what, sizeof what, "anon THP import (%ld MiB huge)", thp / 1024);
        run(b, what);
    }
    return 0;
}
