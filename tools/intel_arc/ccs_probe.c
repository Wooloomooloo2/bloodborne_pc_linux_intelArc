// Does ANV make the port's render targets compressible? Creates 1680x1048 images the way bbport
// does (MUTABLE_FORMAT | EXTENDED_USAGE, color + storage + sampled + transfer, no format list) and
// variants, and prints what ANV asks for: size, alignment and memory type bits. On Xe2+ a
// compressible image is restricted to ANV's compressed memory types, so its bits differ.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

static VkDevice dev;

static void probe(const char* what, VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags,
                  const VkFormat* list, uint32_t list_count) {
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, list_count, list};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, list ? &fl : NULL, flags, VK_IMAGE_TYPE_2D, format,
                            {1680, 1048, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkResult r = vkCreateImage(dev, &ci, NULL, &img);
    if (r) { printf("  %-58s create failed %d\n", what, r); return; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    printf("  %-58s size %8llu KiB  align %7llu  type bits 0x%02x\n", what,
           (unsigned long long)(req.size >> 10), (unsigned long long)req.alignment, req.memoryTypeBits);
    vkDestroyImage(dev, img, NULL);
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ccs_probe", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t c = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &c, pds);
    VkPhysicalDevice pd = 0;
    for (uint32_t i = 0; i < c; ++i) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.vendorID == 0x8086) pd = pds[i]; }
    if (!pd) return 1;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) printf("memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
    float prio = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;

    const VkImageUsageFlags port = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags no_storage = port & ~VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageCreateFlags mut = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    struct { const char* name; VkFormat f; VkFormat alt; } fmts[] = {
        {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT},
        {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
        {"B10G11R11_UFLOAT", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R32_UINT},
    };
    for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); ++i) {
        printf("%s\n", fmts[i].name);
        VkFormat one[1] = {fmts[i].f};
        VkFormat two[2] = {fmts[i].f, fmts[i].alt};
        probe("as bbport: mutable+extended, storage, no format list", fmts[i].f, port, mut, NULL, 0);
        probe("mutable+extended, storage, format list {itself}", fmts[i].f, port, mut, one, 1);
        probe("mutable+extended, storage, format list {itself, alt}", fmts[i].f, port, mut, two, 2);
        probe("mutable+extended, no storage, no format list", fmts[i].f, no_storage, mut, NULL, 0);
        probe("not mutable, storage", fmts[i].f, port, 0, NULL, 0);
        probe("not mutable, no storage (plain render target)", fmts[i].f, no_storage, 0, NULL, 0);
    }
    printf("D32_SFLOAT_S8_UINT\n");
    probe("depth: mutable+extended (as bbport)", VK_FORMAT_D32_SFLOAT_S8_UINT,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, mut, NULL, 0);
    probe("depth: plain", VK_FORMAT_D32_SFLOAT_S8_UINT,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, NULL, 0);
    return 0;
}
