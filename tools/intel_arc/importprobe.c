// Times VK_EXT_external_memory_host imports by the state of the pages (memfd or private;
// written, only read, untouched) at several sizes: how the xe kernel driver's userptr pinning
// scales (quadratic for untouched memfd pages on Linux 7.x). Build: gcc -O2 -o importprobe
// importprobe.c -lvulkan; ./importprobe [max MiB, default 256].
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static VkDevice dev;
static PFN_vkGetMemoryHostPointerPropertiesEXT get_props;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double import_ms(char* p, size_t size, size_t piece) {
    VkMemoryHostPointerPropertiesEXT props = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (get_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &props) || !props.memoryTypeBits) {
        printf("no host pointer types\n");
        exit(1);
    }
    uint32_t type = __builtin_ctz(props.memoryTypeBits);
    size_t n = size / piece;
    VkDeviceMemory* mem = calloc(n, sizeof *mem);
    double t0 = now_ms();
    for (size_t i = 0; i < n; i++) {
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p + i * piece};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, piece, type};
        VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem[i]);
        if (r) {
            printf("vkAllocateMemory: %d\n", r);
            exit(1);
        }
    }
    double t = now_ms() - t0;
    for (size_t i = 0; i < n; i++) vkFreeMemory(dev, mem[i], NULL);
    free(mem);
    return t;
}

int main(int argc, char** argv) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t count = 8;
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &count, pds);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(pds[i], &pr);
        if (pr.vendorID == 0x8086) { pd = pds[i]; printf("GPU: %s\n", pr.deviceName); }
    }
    if (!pd) return 1;
    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char* ext[] = {"VK_EXT_external_memory_host"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
                              .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;
    get_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");

    size_t max = (size_t)(argc > 1 ? atoi(argv[1]) : 256) << 20;
    // Page states before the import: written, only read, untouched; memfd (shared) and private.
    static const char* names[] = {"memfd written", "memfd read", "memfd untouched", "private written",
                                  "private read", "private untouched"};
    for (int mode = 0; mode < 6; mode++) {
        for (size_t mib = 32; (mib << 20) <= max; mib *= 4) {
            size_t s = mib << 20;
            char* p;
            if (mode < 3) {
                int fd = memfd_create("importprobe", 0);
                if (ftruncate(fd, s)) return 1;
                p = mmap(NULL, s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
            } else {
                p = mmap(NULL, s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            }
            volatile char sink = 0;
            for (size_t i = 0; i < s; i += 4096) {
                if (mode % 3 == 0) p[i] = 1;
                else if (mode % 3 == 1) sink += p[i];
            }
            printf("%-18s %5zu MiB: %9.1f ms\n", names[mode], mib, import_ms(p, s, s));
            fflush(stdout);
            munmap(p, s);
        }
    }
    return 0;
}
