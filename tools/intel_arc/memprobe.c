// Replays bbport's guest-memory startup checks on this GPU, outside the port.
// 1) dma-buf export from each HOST_VISIBLE|HOST_CACHED type (incl. DEVICE_LOCAL ones), mmap of the
//    fd whole and at an offset, aliasing, CPU read speed.
// 2) VK_EXT_external_memory_host import of a memfd MAP_SHARED mapping and of anonymous private memory.
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void dmabuf_test(uint32_t type) {
    const VkDeviceSize Size = 2 << 20, Offset = 1 << 20;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext, 0, Size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf; VkResult r = vkCreateBuffer(dev, &bci, NULL, &buf);
    if (r) { printf("  type %u: createBuffer %d\n", type, r); return; }
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << type))) { printf("  type %u: not allowed for the buffer\n", type); vkDestroyBuffer(dev, buf, NULL); return; }
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, type};
    VkDeviceMemory mem; r = vkAllocateMemory(dev, &ai, NULL, &mem);
    if (r) { printf("  type %u: allocate exportable %d\n", type, r); vkDestroyBuffer(dev, buf, NULL); return; }
    PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1; r = getfd(dev, &gi, &fd);
    if (r) { printf("  type %u: export fd %d\n", type, r); goto out; }
    void* whole = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Offset);
    printf("  type %u: exported fd ok; mmap whole %s, mmap at offset %s", type,
           whole == MAP_FAILED ? "FAILED" : "ok", page == MAP_FAILED ? "FAILED" : "ok");
    if (whole != MAP_FAILED && page != MAP_FAILED) {
        *(volatile uint32_t*)page = 0x5ca1ab1e;
        printf(", alias %s", ((volatile uint32_t*)whole)[Offset / 4] == 0x5ca1ab1e ? "ok" : "MISMATCH");
    }
    if (whole != MAP_FAILED) {
        volatile uint64_t* p = whole; uint64_t sum = 0; double t0 = now();
        for (int rep = 0; rep < 16; ++rep) for (size_t i = 0; i < Size / 8; ++i) sum += p[i];
        double s = now() - t0; printf(", CPU read %.0f MB/s%s", 16.0 * Size / s / 1e6, sum == 42 ? "!" : "");
        munmap(whole, Size);
    }
    if (page != MAP_FAILED) munmap(page, 4096);
    printf("\n");
    close(fd);
out:
    vkFreeMemory(dev, mem, NULL); vkDestroyBuffer(dev, buf, NULL);
}

static void host_test(const char* what, void* ptr, VkDeviceSize size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props_fn =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = props_fn(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp);
    printf("  %s: host pointer properties %d, memoryTypeBits 0x%x", what, r, hp.memoryTypeBits);
    if (r || !hp.memoryTypeBits) { printf("\n"); return; }
    uint32_t type = __builtin_ctz(hp.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, type};
    VkDeviceMemory mem; r = vkAllocateMemory(dev, &ai, NULL, &mem);
    printf(", import into type %u: %d\n", type, r);
    if (!r) vkFreeMemory(dev, mem, NULL);
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memprobe", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) { puts("no instance"); return 1; }
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x8086) { pd = pds[i]; printf("%s, driver %u.%u.%u\n", p.deviceName,
            VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion)); }
    }
    if (!pd) { puts("no Intel GPU"); return 1; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char* exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_external_memory_host"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 3, exts};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { puts("no device"); return 1; }

    puts("1) dma-buf export of cached host-visible memory (bbport's DmaBufWorks; it skips DEVICE_LOCAL types):");
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            printf("  (type %u flags 0x%x%s)\n", t, f, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? ", DEVICE_LOCAL: bbport skips it" : "");
            dmabuf_test(t);
        }
    }
    puts("2) host memory import (bbport's HostImportWorks / PrivateImportWorks):");
    const size_t Size = 4 << 20;
    int fd = memfd_create("probe", MFD_CLOEXEC); ftruncate(fd, Size);
    void* shared = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    host_test("memfd MAP_SHARED", shared, Size);
    void* priv = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(priv, 0, Size);
    host_test("anonymous MAP_PRIVATE", priv, Size);
    void* al = aligned_alloc(1 << 16, Size); memset(al, 0, Size);
    host_test("malloc'd (aligned_alloc)", al, Size);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return 0;
}
