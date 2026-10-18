/* wmma32: bit-exact checks of the 16x16 cooperative matrices D3D12 WMMA (FSR4 through vkd3d-proton) needs,
 * at subgroup size 16 and 32 (one subgroup per workgroup, as AMD wave32), on the Vulkan driver in use.
 * Inputs are small integers so every result is exact and the comparison is bit for bit.
 *   ./build.sh && VK_DRIVER_FILES=<icd.json> ./wmma32 [sg ...]          (default: 32 16) */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

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
   memset(b.map, 0, size);
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

static unsigned rng = 12345;
static int rnd(int lo, int hi) { rng = rng * 1103515245u + 12345u; return lo + (int)((rng >> 16) % (unsigned)(hi - lo + 1)); }

enum kind { MMA, FP8EMU, ACC8 };
struct test { const char *spv; enum kind kind; int bcm, dcm, acm, ocm, icm; };

/* buffers: A (binding 0), B (1), C (2), D (3); returns the number of mismatching elements */
static int
run(const struct test *t, VkPhysicalDevice pd, VkQueue q, uint32_t qf, uint32_t sg)
{
   struct buf A = mkbuf(16 * 64 * 2), B = mkbuf(2 * 64 * 16 * 2), C = mkbuf(16 * 16 * 4), D = mkbuf(16 * 16 * 4);
   float ref[256]; uint8_t ref8[256];
   int a[16][64], bm[64][16], c[16][16];

   if (t->kind == MMA) {
      _Float16 *ha = A.map, *hb = B.map; float *fc = C.map;
      for (int r = 0; r < 16; r++) for (int k = 0; k < 64; k++) ha[r * 64 + k] = a[r][k] = rnd(-3, 3);
      for (int k = 0; k < 64; k++) for (int n = 0; n < 16; n++) {
         bm[k][n] = rnd(-3, 3);
         hb[k * 16 + n] = bm[k][n];            /* row-major 64x16 */
         hb[64 * 16 + n * 64 + k] = bm[k][n];  /* its transpose, for the column-major load */
      }
      for (int r = 0; r < 16; r++) for (int n = 0; n < 16; n++) fc[r * 16 + n] = c[r][n] = rnd(-50, 50);
      for (int r = 0; r < 16; r++) for (int n = 0; n < 16; n++) {
         int s = c[r][n];
         for (int k = 0; k < 64; k++) s += a[r][k] * bm[k][n];
         ref[t->dcm ? n * 16 + r : r * 16 + n] = s;
      }
   } else if (t->kind == FP8EMU) {
      uint8_t *ua = A.map, *ub = B.map; float *fc = C.map;
      for (int r = 0; r < 16; r++) for (int k = 0; k < 16; k++) {
         a[r][k] = rnd(120, 136);
         ua[t->acm ? k * 16 + r : r * 16 + k] = a[r][k];
      }
      for (int k = 0; k < 16; k++) for (int n = 0; n < 16; n++) {
         bm[k][n] = rnd(120, 136);
         ub[t->bcm ? n * 16 + k : k * 16 + n] = bm[k][n];
      }
      for (int r = 0; r < 16; r++) for (int n = 0; n < 16; n++) fc[r * 16 + n] = c[r][n] = rnd(-512, 512);
      for (int r = 0; r < 16; r++) for (int n = 0; n < 16; n++) {
         int s = c[r][n];
         for (int k = 0; k < 16; k++) s += (a[r][k] - 128) * (bm[k][n] - 128);
         float v = (float)(_Float16)(float)s / 64.0f + 128.0f;
         v = v < 0 ? 0 : v > 255 ? 255 : v;
         ref8[t->ocm ? n * 16 + r : r * 16 + n] = (uint8_t)v;
      }
   } else {
      uint8_t *ua = A.map;
      for (int r = 0; r < 16; r++) for (int n = 0; n < 16; n++) {
         c[r][n] = rnd(0, 255);
         ua[t->icm ? n * 16 + r : r * 16 + n] = c[r][n];
         ref8[r * 16 + n] = (uint8_t)((3 * c[r][n]) & 255);
      }
   }

   size_t spv_size;
   uint32_t *spv = readfile(t->spv, &spv_size);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo) { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spv_size, .pCode = spv }, NULL, &sm));
   VkDescriptorSetLayoutBinding binds[4];
   for (int i = 0; i < 4; i++)
      binds[i] = (VkDescriptorSetLayoutBinding) { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = binds }, NULL, &dsl));
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo) { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &dsl }, NULL, &pl));
   VkSpecializationMapEntry me = { 0, 0, 4 };
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = &(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = sg },
         .flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main",
         .pSpecializationInfo = &(VkSpecializationInfo) { 1, &me, 4, &sg } } }, NULL, &pipe));
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize) { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 } }, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo) { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ds));
   struct buf *bs[4] = { &A, &B, &C, &D };
   for (int i = 0; i < 4; i++)
      vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ds, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &(VkDescriptorBufferInfo) { bs[i]->b, 0, VK_WHOLE_SIZE } }, 0, NULL);
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = qf }, NULL, &cp));
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, 1, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb }, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   int bad = 0, first = -1;
   for (int i = 0; i < 256; i++) {
      int ok = t->kind == MMA ? ((float *)D.map)[i] == ref[i] : ((uint8_t *)D.map)[i] == ref8[i];
      if (!ok) { bad++; if (first < 0) first = i; }
   }
   if (bad) {
      if (t->kind == MMA)
         printf("    first mismatch [%d]: got %g want %g\n", first, ((float *)D.map)[first], ref[first]);
      else
         printf("    first mismatch [%d]: got %u want %u\n", first, ((uint8_t *)D.map)[first], ref8[first]);
   }
   vkDestroyPipeline(dev, pipe, NULL);
   vkDestroyShaderModule(dev, sm, NULL);
   free(spv);
   return bad;
}

int
main(int argc, char **argv)
{
   uint32_t sgs[4] = { 32, 16 }, nsg = 2;
   if (argc > 1) { nsg = 0; for (int i = 1; i < argc && nsg < 4; i++) sgs[nsg++] = atoi(argv[i]); }

   VkInstance inst;
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo) { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &(VkApplicationInfo) { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 } },
      NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   printf("device: %s\n", props.deviceName);

   VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
      .cooperativeMatrix = VK_TRUE };
   VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &cmf,
      .subgroupSizeControl = VK_TRUE, .computeFullSubgroups = VK_TRUE };
   VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13,
      .shaderFloat16 = VK_TRUE, .shaderInt8 = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE, .vulkanMemoryModel = VK_TRUE };
   VkPhysicalDeviceVulkan11Features v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12,
      .storageBuffer16BitAccess = VK_TRUE };
   const char *ext = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo) { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v11,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
         .pQueuePriorities = &(float) { 1 } },
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext }, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   static const struct test tests[] = {
      { "mma.spv", MMA }, { "mma_bcm.spv", MMA, .bcm = 1 }, { "mma_dcm.spv", MMA, .dcm = 1 },
      { "mma_bcm_dcm.spv", MMA, .bcm = 1, .dcm = 1 },
      { "fp8emu.spv", FP8EMU }, { "fp8emu_acm.spv", FP8EMU, .acm = 1 }, { "fp8emu_bcm.spv", FP8EMU, .bcm = 1 },
      { "fp8emu_ocm.spv", FP8EMU, .ocm = 1 }, { "fp8emu_acm_bcm_ocm.spv", FP8EMU, .acm = 1, .bcm = 1, .ocm = 1 },
      { "acc8.spv", ACC8 }, { "acc8_icm.spv", ACC8, .icm = 1 },
   };
   int fails = 0;
   for (uint32_t s = 0; s < nsg; s++)
      for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
         int bad = run(&tests[i], pd, q, 0, sgs[s]);
         printf("%-26s sg%-2u %s", tests[i].spv, sgs[s], bad ? "FAIL" : "ok");
         if (bad) printf(" (%d/256 wrong)", bad);
         printf("\n");
         fails += bad != 0;
      }
   printf("%s: %d failing\n", fails ? "FAIL" : "PASS", fails);
   return fails != 0;
}
