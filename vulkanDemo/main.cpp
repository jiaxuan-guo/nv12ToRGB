
// ===== main.cpp =====
// Minimal Vulkan program that:
// - takes an input NV12 raw file (width=640, height=480 by default)
// - uploads Y plane and UV plane to Vulkan images
// - runs two compute shaders (Y and UV) to scale to output size (default 320x240)
// - downloads scaled images and writes a raw NV12 file (Y plane then interleaved UV as UVUV...)

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iostream>
#include <cassert>

static void die(const char* msg) { std::cerr<<msg<<""; std::exit(1); }
static std::vector<char> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f) die("failed to open file");
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

// helper to create Vulkan instance/device/etc. This example keeps things minimal and assumes
// a Vulkan-capable driver (Mesa) is available. Not production hardened.

int main(int argc, char** argv) {
    // Defaults: input 640x480 -> output 320x240
    const char* inPath = "input_nv12.raw";
    int inW = 640, inH = 480;
    int outW = 320, outH = 240;
    const char* spvY = "compute_y.spv";
    const char* spvUV = "compute_uv.spv";
    const char* outPath = "scaled_nv12.raw";

    if (argc >= 2) inPath = argv[1];
    if (argc >= 4) { inW = atoi(argv[2]); inH = atoi(argv[3]); }
    if (argc >= 6) { outW = atoi(argv[4]); outH = atoi(argv[5]); }
    if (argc >= 7) spvY = argv[6];
    if (argc >= 8) spvUV = argv[7];
    if (argc >= 9) outPath = argv[8];

    if (inW % 2 != 0 || inH % 2 != 0 || outW % 2 != 0 || outH % 2 != 0) die("width and height must be even for NV12");

    size_t inSize = size_t(inW) * size_t(inH) * 3 / 2;
    std::vector<uint8_t> nv12(inSize);
    std::ifstream inf(inPath, std::ios::binary);
    if(!inf) die("failed open input nv12");
    inf.read((char*)nv12.data(), inSize);
    if (inf.gcount() != (std::streamsize)inSize) die("input size mismatch");

    size_t ySize = size_t(inW) * size_t(inH);
    size_t uvSize = size_t(inW/2) * size_t(inH/2) * 2; // interleaved UV
    uint8_t* yPtr = nv12.data();
    uint8_t* uvPtr = nv12.data() + ySize;

    // Vulkan init
    VkInstance instance;
    {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "nv12_scaler";
        ai.applicationVersion = VK_MAKE_VERSION(1,0,0);
        ai.pEngineName = "none";
        ai.engineVersion = VK_MAKE_VERSION(1,0,0);
        ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &ai;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) die("vkCreateInstance failed");
    }

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) die("no GPU with Vulkan support");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    VkPhysicalDevice physical = gpus[0];

    uint32_t qfCount=0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qfCount, qfs.data());
    int qfi = -1;
    for (int i=0;i<(int)qfCount;i++) if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
    if (qfi < 0) die("no compute queue");

    VkDevice device; VkQueue queue;
    {
        float pr = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &pr;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) die("vkCreateDevice failed");
        vkGetDeviceQueue(device, qfi, 0, &queue);
    }

    VkCommandPool cmdPool;
    {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfi;
        if (vkCreateCommandPool(device, &pci, nullptr, &cmdPool) != VK_SUCCESS) die("cmdpool create fail");
    }

    auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags props)->uint32_t {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i=0;i<mp.memoryTypeCount;i++){
            if ((typeFilter & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
        }
        die("no suitable memory type");
        return 0;
    };

    auto createImage = [&](int w,int h,VkFormat fmt,VkImageUsageFlags usage, VkImage& img, VkDeviceMemory& mem){
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1; ici.format = fmt; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ici.usage = usage; ici.samples = VK_SAMPLE_COUNT_1_BIT;
        if (vkCreateImage(device, &ici, nullptr, &img) != VK_SUCCESS) die("createImage fail");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, img, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS) die("alloc mem fail");
        vkBindImageMemory(device, img, mem, 0);
    };

    // create images: input Y/UV (we'll upload) and output Y/UV
    VkImage imgY, imgUV, outY, outUV; VkDeviceMemory memY, memUV, memOutY, memOutUV;
    createImage(inW, inH, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, imgY, memY);
    createImage(inW/2, inH/2, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, imgUV, memUV);
    createImage(outW, outH, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, outY, memOutY);
    createImage(outW/2, outH/2, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, outUV, memOutUV);

    auto createImageView = [&](VkImage image, VkFormat fmt, VkImageView& view){
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; ivci.image = image; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = fmt; ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ivci.subresourceRange.levelCount = 1; ivci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &ivci, nullptr, &view) != VK_SUCCESS) die("create view fail");
    };
    VkImageView viewY, viewUV, viewOutY, viewOutUV;
    createImageView(imgY, VK_FORMAT_R8_UNORM, viewY);
    createImageView(imgUV, VK_FORMAT_R8G8_UNORM, viewUV);
    createImageView(outY, VK_FORMAT_R8_UNORM, viewOutY);
    createImageView(outUV, VK_FORMAT_R8G8_UNORM, viewOutUV);

    // staging buffers
    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, VkMemoryPropertyFlags props){
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS) die("createBuffer fail");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, buf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, props);
        if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS) die("alloc buf mem fail");
        vkBindBufferMemory(device, buf, mem, 0);
    };

    VkBuffer stgY, stgUV, stgOutY, stgOutUV; VkDeviceMemory stgYmem, stgUVmem, stgOutYmem, stgOutUVmem;
    createBuffer(ySize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stgY, stgYmem, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    createBuffer(uvSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stgUV, stgUVmem, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    createBuffer((size_t)outW*outH, VK_BUFFER_USAGE_TRANSFER_DST_BIT, stgOutY, stgOutYmem, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    createBuffer((size_t)(outW/2)*(outH/2)*2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, stgOutUV, stgOutUVmem, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // fill staging input
    void* p; vkMapMemory(device, stgYmem, 0, VK_WHOLE_SIZE, 0, &p); memcpy(p, yPtr, ySize); vkUnmapMemory(device, stgYmem);
    vkMapMemory(device, stgUVmem, 0, VK_WHOLE_SIZE, 0, &p); memcpy(p, uvPtr, uvSize); vkUnmapMemory(device, stgUVmem);

    // command buffer
    VkCommandBuffer cmd;
    { VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1; if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) die("alloc cb"); }

    auto beginSingle = [&](){ VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(cmd, &bbi); };
    auto endSingle = [&](){ vkEndCommandBuffer(cmd); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd; vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue); vkResetCommandBuffer(cmd, 0); };

    auto setImageLayout = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange range){
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout = oldLayout; barrier.newLayout = newLayout; barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.image = image; barrier.subresourceRange = range;
        barrier.srcAccessMask = 0; barrier.dstAccessMask = 0; VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) { barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT; }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) { barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT; dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; }
        else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) { barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT; }
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) { barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; }
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    // transition inputs and outputs
    beginSingle();
    VkImageSubresourceRange rY{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange rUV{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    setImageLayout(imgY, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rY);
    setImageLayout(imgUV, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rUV);
    setImageLayout(outY, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, rY);
    setImageLayout(outUV, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, rUV);
    endSingle();

    // copy staging -> images
    beginSingle();
    VkBufferImageCopy bicY{}; bicY.bufferOffset = 0; bicY.bufferRowLength = 0; bicY.bufferImageHeight = 0; bicY.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bicY.imageSubresource.mipLevel = 0; bicY.imageSubresource.baseArrayLayer = 0; bicY.imageSubresource.layerCount = 1; bicY.imageOffset = {0,0,0}; bicY.imageExtent = {(uint32_t)inW,(uint32_t)inH,1};
    vkCmdCopyBufferToImage(cmd, stgY, imgY, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bicY);
    VkBufferImageCopy bicUV{}; bicUV.bufferOffset = 0; bicUV.bufferRowLength = 0; bicUV.bufferImageHeight = 0; bicUV.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bicUV.imageSubresource.mipLevel = 0; bicUV.imageSubresource.baseArrayLayer = 0; bicUV.imageSubresource.layerCount = 1; bicUV.imageOffset = {0,0,0}; bicUV.imageExtent = {(uint32_t)(inW/2),(uint32_t)(inH/2),1};
    vkCmdCopyBufferToImage(cmd, stgUV, imgUV, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bicUV);
    endSingle();

    // transition inputs to GENERAL for compute
    beginSingle();
    setImageLayout(imgY, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, rY);
    setImageLayout(imgUV, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, rUV);
    endSingle();

    // helper to create compute pipeline for a shader
    auto createComputePipeline = [&](const char* spvPath, VkPipelineLayout playout, VkPipeline& pipeline){
        auto spv = readFile(spvPath);
        VkShaderModule compMod; VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smci.codeSize = spv.size(); smci.pCode = reinterpret_cast<const uint32_t*>(spv.data()); if (vkCreateShaderModule(device, &smci, nullptr, &compMod) != VK_SUCCESS) die("create shader module fail");
        VkPipelineShaderStageCreateInfo pss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pss.stage = VK_SHADER_STAGE_COMPUTE_BIT; pss.module = compMod; pss.pName = "main";
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpci.stage = pss; cpci.layout = playout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) != VK_SUCCESS) die("create comp pipeline fail");
        vkDestroyShaderModule(device, compMod, nullptr);
    };

    // create descriptor layouts and pipelines for Y and UV
    VkDescriptorSetLayout dslY, dslUV;
    {
        VkDescriptorSetLayoutBinding by[2];
        by[0].binding = 0; by[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; by[0].descriptorCount = 1; by[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; by[0].pImmutableSamplers = nullptr;
        by[1].binding = 1; by[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; by[1].descriptorCount = 1; by[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; by[1].pImmutableSamplers = nullptr;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dslci.bindingCount = 2; dslci.pBindings = by;
        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dslY) != VK_SUCCESS) die("create dslY fail");
        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dslUV) != VK_SUCCESS) die("create dslUV fail");
    }

    VkPipelineLayout plY, plUV;
    {
        VkPushConstantRange pcY{}; pcY.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcY.offset = 0; pcY.size = sizeof(int)*4;
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount = 1; plci.pSetLayouts = &dslY; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcY; if (vkCreatePipelineLayout(device, &plci, nullptr, &plY) != VK_SUCCESS) die("create plY fail");
        VkPushConstantRange pcUV{}; pcUV.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcUV.offset = 0; pcUV.size = sizeof(int)*4;
        VkPipelineLayoutCreateInfo plci2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci2.setLayoutCount = 1; plci2.pSetLayouts = &dslUV; plci2.pushConstantRangeCount = 1; plci2.pPushConstantRanges = &pcUV; if (vkCreatePipelineLayout(device, &plci2, nullptr, &plUV) != VK_SUCCESS) die("create plUV fail");
    }

    VkDescriptorPool dpool;
    {
        VkDescriptorPoolSize ps[1]; ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps[0].descriptorCount = 4;
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpci.maxSets = 2; dpci.poolSizeCount = 1; dpci.pPoolSizes = ps; if (vkCreateDescriptorPool(device, &dpci, nullptr, &dpool) != VK_SUCCESS) die("create dpool fail");
    }

    // allocate and update descriptor sets
    VkDescriptorSet dsetY, dsetUV;
    {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dslY; if (vkAllocateDescriptorSets(device, &dsai, &dsetY) != VK_SUCCESS) die("alloc dsetY fail");
        VkDescriptorImageInfo diiY[2]; diiY[0].sampler = VK_NULL_HANDLE; diiY[0].imageView = viewY; diiY[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL; diiY[1].sampler = VK_NULL_HANDLE; diiY[1].imageView = viewOutY; diiY[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet wdsY[2]; for (int i=0;i<2;i++){ wdsY[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wdsY[i].dstSet = dsetY; wdsY[i].dstBinding = i; wdsY[i].dstArrayElement = 0; wdsY[i].descriptorCount = 1; wdsY[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wdsY[i].pImageInfo = &diiY[i]; wdsY[i].pBufferInfo = nullptr; wdsY[i].pTexelBufferView = nullptr; }
        vkUpdateDescriptorSets(device, 2, wdsY, 0, nullptr);

        VkDescriptorSetAllocateInfo dsai2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai2.descriptorPool = dpool; dsai2.descriptorSetCount = 1; dsai2.pSetLayouts = &dslUV; if (vkAllocateDescriptorSets(device, &dsai2, &dsetUV) != VK_SUCCESS) die("alloc dsetUV fail");
        VkDescriptorImageInfo diiUV[2]; diiUV[0].sampler = VK_NULL_HANDLE; diiUV[0].imageView = viewUV; diiUV[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL; diiUV[1].sampler = VK_NULL_HANDLE; diiUV[1].imageView = viewOutUV; diiUV[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet wdsUV[2]; for (int i=0;i<2;i++){ wdsUV[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wdsUV[i].dstSet = dsetUV; wdsUV[i].dstBinding = i; wdsUV[i].dstArrayElement = 0; wdsUV[i].descriptorCount = 1; wdsUV[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wdsUV[i].pImageInfo = &diiUV[i]; wdsUV[i].pBufferInfo = nullptr; wdsUV[i].pTexelBufferView = nullptr; }
        vkUpdateDescriptorSets(device, 2, wdsUV, 0, nullptr);
    }

    // create pipelines
    VkPipeline pipeY, pipeUV; createComputePipeline(spvY, plY, pipeY); createComputePipeline(spvUV, plUV, pipeUV);

    // dispatch Y compute
    beginSingle();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeY);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plY, 0, 1, &dsetY, 0, nullptr);
    int pushY[4] = { inW, inH, outW, outH };
    vkCmdPushConstants(cmd, plY, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushY), pushY);
    uint32_t gx = (outW + 15) / 16; uint32_t gy = (outH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);
    endSingle();

    // dispatch UV compute (operate on half resolution planes)
    beginSingle();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeUV);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plUV, 0, 1, &dsetUV, 0, nullptr);
    int inWuv = inW/2, inHuv = inH/2, outWuv = outW/2, outHuv = outH/2;
    int pushUV[4] = { inWuv, inHuv, outWuv, outHuv };
    vkCmdPushConstants(cmd, plUV, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushUV), pushUV);
    uint32_t gx2 = (outWuv + 15) / 16; uint32_t gy2 = (outHuv + 15) / 16;
    vkCmdDispatch(cmd, gx2, gy2, 1);
    endSingle();

    // transition out images to TRANSFER_SRC and copy to host buffers
    beginSingle();
    setImageLayout(outY, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rY);
    setImageLayout(outUV, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rUV);
    endSingle();

    beginSingle();
    VkBufferImageCopy bicOutY{}; bicOutY.bufferOffset = 0; bicOutY.bufferRowLength = 0; bicOutY.bufferImageHeight = 0; bicOutY.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bicOutY.imageSubresource.mipLevel = 0; bicOutY.imageSubresource.baseArrayLayer = 0; bicOutY.imageSubresource.layerCount = 1; bicOutY.imageOffset = {0,0,0}; bicOutY.imageExtent = {(uint32_t)outW,(uint32_t)outH,1};
    vkCmdCopyImageToBuffer(cmd, outY, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stgOutY, 1, &bicOutY);
    VkBufferImageCopy bicOutUV{}; bicOutUV.bufferOffset = 0; bicOutUV.bufferRowLength = 0; bicOutUV.bufferImageHeight = 0; bicOutUV.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bicOutUV.imageSubresource.mipLevel = 0; bicOutUV.imageSubresource.baseArrayLayer = 0; bicOutUV.imageSubresource.layerCount = 1; bicOutUV.imageOffset = {0,0,0}; bicOutUV.imageExtent = {(uint32_t)outWuv,(uint32_t)outHuv,1};
    vkCmdCopyImageToBuffer(cmd, outUV, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stgOutUV, 1, &bicOutUV);
    endSingle();

    // map buffers and write NV12 raw: Y plane then interleaved UV (U,V pairs)
    void* outpY; vkMapMemory(device, stgOutYmem, 0, VK_WHOLE_SIZE, 0, &outpY);
    void* outpUV; vkMapMemory(device, stgOutUVmem, 0, VK_WHOLE_SIZE, 0, &outpUV);

    std::ofstream outf(outPath, std::ios::binary);
    if (!outf) die("failed to open output file");
    // write Y
    outf.write((char*)outpY, (std::streamsize)outW * outH);
    // write UV: outpUV is contiguous RG bytes (U,V,U,V...). NV12 expects interleaved UV exactly like that.
    outf.write((char*)outpUV, (std::streamsize)(outWuv * outHuv * 2));
    outf.close();

    vkUnmapMemory(device, stgOutYmem);
    vkUnmapMemory(device, stgOutUVmem);

    std::cout<<"Wrote scaled NV12 to "<<outPath<<" ("<<outW<<"x"<<outH<<")"<<std::endl;

    // cleanup (omitted many destroys for brevity) - in a demo it's OK to let OS reclaim at exit
    vkDestroyPipeline(device, pipeY, nullptr); vkDestroyPipeline(device, pipeUV, nullptr);
    vkDestroyPipelineLayout(device, plY, nullptr); vkDestroyPipelineLayout(device, plUV, nullptr);
    vkDestroyDescriptorPool(device, dpool, nullptr);
    vkDestroyDescriptorSetLayout(device, dslY, nullptr); vkDestroyDescriptorSetLayout(device, dslUV, nullptr);

    vkDestroyBuffer(device, stgY, nullptr); vkFreeMemory(device, stgYmem, nullptr);
    vkDestroyBuffer(device, stgUV, nullptr); vkFreeMemory(device, stgUVmem, nullptr);
    vkDestroyBuffer(device, stgOutY, nullptr); vkFreeMemory(device, stgOutYmem, nullptr);
    vkDestroyBuffer(device, stgOutUV, nullptr); vkFreeMemory(device, stgOutUVmem, nullptr);

    vkDestroyImageView(device, viewY, nullptr); vkDestroyImage(device, imgY, nullptr); vkFreeMemory(device, memY, nullptr);
    vkDestroyImageView(device, viewUV, nullptr); vkDestroyImage(device, imgUV, nullptr); vkFreeMemory(device, memUV, nullptr);
    vkDestroyImageView(device, viewOutY, nullptr); vkDestroyImage(device, outY, nullptr); vkFreeMemory(device, memOutY, nullptr);
    vkDestroyImageView(device, viewOutUV, nullptr); vkDestroyImage(device, outUV, nullptr); vkFreeMemory(device, memOutUV, nullptr);

    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
