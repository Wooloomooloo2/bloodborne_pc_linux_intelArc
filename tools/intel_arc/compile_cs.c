// Compiles one compute SPIR-V (one storage buffer at set 0 binding 0) on the Intel GPU, so that
// INTEL_DEBUG output can be matched to a known module.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: compile_cs file.spv\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned* code = malloc(n); fread(code, 1, n, f); fclose(f);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "compile_cs", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t c = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &c, pds);
    VkPhysicalDevice pd = 0;
    for (uint32_t i = 0; i < c; ++i) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.vendorID == 0x8086) pd = pds[i]; }
    if (!pd) return 1;
    float prio = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q};
    VkDevice dev; if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)n, code};
    VkShaderModule sm; if (vkCreateShaderModule(dev, &smci, NULL, &sm)) return 1;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl);
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl};
    VkPipelineLayout layout; vkCreatePipelineLayout(dev, &pl, NULL, &layout);
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, layout};
    VkPipeline pipe; VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipe);
    fprintf(stderr, "compile_cs: vkCreateComputePipelines %d\n", r);
    return r ? 1 : 0;
}
