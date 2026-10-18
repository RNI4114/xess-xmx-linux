/* dot4dpas: runs a gen.py shader (64 invocations per workgroup, binding 0 = input dwords, binding 1 = output) and
 * compares the output bit for bit with the CPU reference.
 *   VK_DRIVER_FILES=<icd.json> ./dot4dpas <case> [required subgroup size]      (INTEL_DOT4_DPAS=0|1) */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define N (64 * 32)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
struct buf { VkBuffer b; VkDeviceMemory m; void *map; };

static struct buf
mkbuf(VkDeviceSize size)
{
   struct buf b;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo) { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &b.b));
   VkMemoryRequirements r;
   vkGetBufferMemoryRequirements(dev, b.b, &r);
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t t = 0;
   while (t < mp.memoryTypeCount && !((r.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want))
      t++;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo) { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = r.size, .memoryTypeIndex = t }, NULL, &b.m));
   CHECK(vkBindBufferMemory(dev, b.b, b.m, 0));
   CHECK(vkMapMemory(dev, b.m, 0, size, 0, &b.map));
   memset(b.map, 0xcd, size);
   return b;
}

static void *
readfile(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
   void *d = malloc(*size);
   if (fread(d, 1, *size, f) != *size) exit(1);
   fclose(f);
   return d;
}

int
main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s <case> [subgroup size]\n", argv[0]); return 2; }
   uint32_t sg = argc > 2 ? atoi(argv[2]) : 0;
   char path[512];
   size_t spv_size, in_size, ref_size;
   snprintf(path, sizeof(path), "%s.spv", argv[1]); uint32_t *spv = readfile(path, &spv_size);
   snprintf(path, sizeof(path), "%s.in", argv[1]); uint32_t *in = readfile(path, &in_size);
   snprintf(path, sizeof(path), "%s.ref", argv[1]); uint32_t *ref = readfile(path, &ref_size);

   VkInstance inst;
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo) { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &(VkApplicationInfo) { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 } },
      NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .subgroupSizeControl = VK_TRUE, .computeFullSubgroups = VK_TRUE, .shaderIntegerDotProduct = VK_TRUE };
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo) { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v13,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
         .pQueuePriorities = &(float) { 1.0f } } }, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   struct buf I = mkbuf(in_size), O = mkbuf(ref_size);
   memcpy(I.map, in, in_size);
   size_t w_size = 4;
   snprintf(path, sizeof(path), "%s.w", argv[1]);
   FILE *wf = fopen(path, "rb");
   uint32_t *wdata = wf ? (fclose(wf), readfile(path, &w_size)) : NULL;
   struct buf Wb = mkbuf(w_size);
   if (wdata) memcpy(Wb.map, wdata, w_size);

   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo) { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spv_size, .pCode = spv }, NULL, &sm));
   VkDescriptorSetLayoutBinding binds[3];
   for (int i = 0; i < 3; i++)
      binds[i] = (VkDescriptorSetLayoutBinding) { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = binds }, NULL, &dsl));
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo) { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &dsl }, NULL, &pl));
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = sg };
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = sg ? &rsg : NULL,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } }, NULL, &pipe));
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize) { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 } }, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo) { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ds));
   struct buf *bs[3] = { &I, &O, &Wb };
   for (int i = 0; i < 3; i++)
      vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ds, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &(VkDescriptorBufferInfo) { bs[i]->b, 0, VK_WHOLE_SIZE } }, 0, NULL);
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0 }, NULL, &cp));
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   unsigned groups = N / 64;
   snprintf(path, sizeof(path), "%s.groups", argv[1]);
   FILE *gf = fopen(path, "r");
   if (gf) { if (fscanf(gf, "%u", &groups) != 1) exit(1); fclose(gf); }
   vkCmdDispatch(cb, groups, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb }, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   const uint32_t *o = O.map;
   if (getenv("DOT4_OUT")) {
      FILE *of = fopen(getenv("DOT4_OUT"), "wb");
      fwrite(o, 1, ref_size, of);
      fclose(of);
   }
   size_t cnt = ref_size / 4, bad = 0, first = 0;
   for (size_t i = 0; i < cnt; i++)
      if (o[i] != ref[i] && !bad++) first = i;
   printf("%-8s sg %2u: %zu/%zu wrong", argv[1], sg, bad, cnt);
   if (bad)
      printf(" (first [%zu]: got 0x%08x want 0x%08x)", first, o[first], ref[first]);
   printf("\n");
   return bad != 0;
}
