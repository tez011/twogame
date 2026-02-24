#define SDL_MAIN_USE_CALLBACKS
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <ktx.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "display.h"
#include "physfs.h"
#include "scene.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

class ktx_mip_iterate_userdata {
private:
    std::vector<VkBufferImageCopy> m_regions;
    uint32_t m_layer_count;
    VkDeviceSize m_offset;

public:
    ktx_mip_iterate_userdata(uint32_t layer_count, VkDeviceSize offset)
        : m_layer_count(layer_count)
        , m_offset(offset)
    {
        m_regions.reserve(layer_count);
    }

    const std::vector<VkBufferImageCopy>& regions() const { return m_regions; }

    void add_region(int miplevel, int face, int width, int height, int depth, ktx_uint64_t face_lod_size)
    {
        VkBufferImageCopy& region = m_regions.emplace_back();
        region.bufferOffset = m_offset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = miplevel;
        region.imageSubresource.baseArrayLayer = face;
        region.imageSubresource.layerCount = m_layer_count;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = depth;
        m_offset += face_lod_size;
    }
};

ktxStream ktx_physfs_istream(PHYSFS_File* fh)
{
    ktxStream stream {};
    stream.type = eStreamTypeCustom;
    stream.read = [](ktxStream* self, void* dst, const ktx_size_t count) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        PHYSFS_sint64 rs = PHYSFS_readBytes(fh, dst, count);
        if (rs < 0)
            return KTX_FILE_READ_ERROR;
        else if (static_cast<ktx_size_t>(rs) < count && PHYSFS_eof(fh))
            return KTX_FILE_UNEXPECTED_EOF;
        else
            return KTX_SUCCESS;
    };
    stream.skip = [](ktxStream* self, const ktx_size_t count) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        PHYSFS_sint64 tell = PHYSFS_tell(fh);
        if (tell == -1)
            return KTX_FILE_SEEK_ERROR;
        if (PHYSFS_seek(fh, tell + count))
            return KTX_SUCCESS;
        if (PHYSFS_getLastErrorCode() == PHYSFS_ERR_PAST_EOF)
            return KTX_FILE_UNEXPECTED_EOF;
        else
            return KTX_FILE_SEEK_ERROR;
    };
    stream.write = nullptr;
    stream.getpos = [](ktxStream* self, ktx_off_t* const offset) {
        if (offset) {
            PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
            PHYSFS_sint64 tell = PHYSFS_tell(fh);
            if (tell == -1)
                return KTX_FILE_SEEK_ERROR;
            *offset = tell;
        }
        return KTX_SUCCESS;
    };
    stream.setpos = [](ktxStream* self, const ktx_off_t offset) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        if (PHYSFS_seek(fh, offset))
            return KTX_SUCCESS;
        if (PHYSFS_getLastErrorCode() == PHYSFS_ERR_PAST_EOF)
            return KTX_FILE_UNEXPECTED_EOF;
        else
            return KTX_FILE_SEEK_ERROR;
    };
    stream.getsize = [](ktxStream* self, ktx_size_t* const size) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        PHYSFS_sint64 sz = PHYSFS_fileLength(fh);
        if (sz == -1)
            return KTX_FILE_DATA_ERROR;
        if (size)
            *size = sz;
        return KTX_SUCCESS;
    };
    stream.destruct = [](ktxStream* self) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        PHYSFS_close(fh);
    };
    stream.data.custom_ptr.address = fh;
    stream.closeOnDestruct = KTX_TRUE;
    return stream;
}

static ktx_error_code_e ktx_mip_iterate(int miplevel, int face, int width, int height, int depth, ktx_uint64_t face_lod_size, void* pixels, void* userdata)
{
    ktx_mip_iterate_userdata* mip_data = reinterpret_cast<ktx_mip_iterate_userdata*>(userdata);
    mip_data->add_region(miplevel, face, width, height, depth, face_lod_size);
    (void)(pixels);
    return KTX_SUCCESS;
}

class DuckScene : public twogame::IScene {
    // Descriptor info, from renderer/shaders. It is not clear to me that this belongs to the scene,
    // but the scene totally does define the data written to these buffers
    static constexpr size_t DESCRIPTOR_SET_COUNT = 2;
    VkDescriptorPool m_descriptor_pool;
    std::array<std::array<VkDescriptorSet, DESCRIPTOR_SET_COUNT>, SIMULTANEOUS_FRAMES> m_descriptor_sets;
    std::array<VkBuffer, SIMULTANEOUS_FRAMES> m_uniform_buffer;
    std::array<VmaAllocation, SIMULTANEOUS_FRAMES> m_uniform_buffer_mem;

    VkBuffer m_buffer;
    VkImage m_image;
    VkImageView m_image_view;
    VkSampler m_sampler;
    VmaAllocation m_mem, m_image_mem;
    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;

public:
    DuckScene(twogame::DisplayHost* host)
        : twogame::IScene(host)
    {
    }
    virtual ~DuckScene();

    virtual bool construct(twogame::IRenderer* renderer, VkCommandBuffer prepare_commands, int pass, VkBuffer staging_buffer, unsigned char* staging_data);
    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage);
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage);
    virtual void record_commands(twogame::IRenderer* renderer, uint32_t frame_number);

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass);
};

DuckScene::~DuckScene()
{
    vkDestroySampler(r_device, m_sampler, nullptr);
    vkDestroyImageView(r_device, m_image_view, nullptr);
    vmaDestroyImage(r_allocator, m_image, m_image_mem);
    vmaDestroyBuffer(r_allocator, m_buffer, m_mem);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(r_device, *it, nullptr);

    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++)
        vmaDestroyBuffer(r_allocator, m_uniform_buffer[i], m_uniform_buffer_mem[i]);
    vkDestroyDescriptorPool(r_device, m_descriptor_pool, nullptr);
}

bool DuckScene::construct(twogame::IRenderer* renderer, VkCommandBuffer prepare_commands, int pass, VkBuffer staging_buffer, unsigned char* staging_data)
{
    size_t staging_offset = 0;
    PHYSFS_File* duckdata = PHYSFS_openRead("/data/duck.bin");
    PHYSFS_File* imagedata = PHYSFS_openRead("/data/duck.i0.ktx2");
    SDL_assert(duckdata);
    SDL_assert(imagedata);

    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = renderer->queue_family_index(twogame::QueueType::Graphics);

    VkCommandBufferAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateCommandPool(r_device, &pool_info, nullptr, &m_draw_cmd_pool[i]));

        alloc_info.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(r_device, &alloc_info, m_draw_cmd[i].data()));
    }

    auto pool_sizes = std::to_array<VkDescriptorPoolSize>({
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SIMULTANEOUS_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SIMULTANEOUS_FRAMES },
    });
    VkDescriptorPoolCreateInfo descriptor_pool_info {};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = DESCRIPTOR_SET_COUNT * SIMULTANEOUS_FRAMES; // ???
    descriptor_pool_info.poolSizeCount = pool_sizes.size();
    descriptor_pool_info.pPoolSizes = pool_sizes.data();
    VK_DEMAND(vkCreateDescriptorPool(r_device, &descriptor_pool_info, nullptr, &m_descriptor_pool));

    auto descriptor_set_layouts = std::to_array<VkDescriptorSetLayout>({
        renderer->descriptor_set_layout(0),
        renderer->descriptor_set_layout(1),
    });
    VkDescriptorSetAllocateInfo descriptor_alloc_info {};
    descriptor_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_alloc_info.descriptorPool = m_descriptor_pool;
    descriptor_alloc_info.descriptorSetCount = DESCRIPTOR_SET_COUNT;
    descriptor_alloc_info.pSetLayouts = descriptor_set_layouts.data();
    VK_DEMAND(vkAllocateDescriptorSets(r_device, &descriptor_alloc_info, m_descriptor_sets[0].data()));
    VK_DEMAND(vkAllocateDescriptorSets(r_device, &descriptor_alloc_info, m_descriptor_sets[1].data()));

    VkImageCreateInfo image_info {};
    VkImageViewCreateInfo image_view_info {};
    VkBufferCreateInfo buffer_info {};
    VmaAllocationCreateInfo mem_createinfo {};
    VmaAllocationInfo mem_info;
    void* mapped_buffer;
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;

    buffer_info.size = sizeof(glm::mat4);
    buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++)
        VK_DEMAND(vmaCreateBuffer(r_allocator, &buffer_info, &mem_createinfo, &m_uniform_buffer[i], &m_uniform_buffer_mem[i], nullptr));

    buffer_info.size = 102040;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_DEMAND(vmaCreateBuffer(r_allocator, &buffer_info, &mem_createinfo, &m_buffer, &m_mem, &mem_info));

    VkMemoryPropertyFlags mem_type_flags;
    vmaGetMemoryTypeProperties(r_allocator, mem_info.memoryType, &mem_type_flags);
    if (mem_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK_DEMAND(vmaMapMemory(r_allocator, m_mem, &mapped_buffer));
        PHYSFS_readBytes(duckdata, mapped_buffer, buffer_info.size);
        vmaUnmapMemory(r_allocator, m_mem);
        if ((mem_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            vmaFlushAllocation(r_allocator, m_mem, 0, VK_WHOLE_SIZE);
    } else {
        VkBufferCopy copy {};
        copy.srcOffset = 0;
        copy.dstOffset = 0;
        copy.size = buffer_info.size;
        PHYSFS_readBytes(duckdata, staging_data + staging_offset, buffer_info.size);
        vkCmdCopyBuffer(prepare_commands, staging_buffer, m_buffer, 1, &copy);
        staging_offset += (buffer_info.size + 15) & ~15;

        if (renderer->queue_family_index(twogame::QueueType::Graphics) != renderer->queue_family_index(twogame::QueueType::Transfer)) {
            VkDependencyInfo dep {};
            VkBufferMemoryBarrier2 barrier {};

            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &barrier;
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
            barrier.srcQueueFamilyIndex = renderer->queue_family_index(twogame::QueueType::Transfer);
            barrier.dstQueueFamilyIndex = renderer->queue_family_index(twogame::QueueType::Graphics);
            barrier.buffer = m_buffer;
            barrier.offset = mem_info.offset;
            barrier.size = mem_info.size;
            vkCmdPipelineBarrier2(prepare_commands, &dep);
        }
    }

    ktxTexture2* ktx2 = nullptr;
    ktxStream kstream = ktx_physfs_istream(imagedata);
    ktx_error_code_e k_res = ktxTexture2_CreateFromStream(&kstream, 0, &ktx2);
    SDL_assert(k_res == KTX_SUCCESS);
    SDL_assert(ktx2->vkFormat);

    ktxTexture* ktx = reinterpret_cast<ktxTexture*>(ktx2);
    SDL_assert(ktx->numDimensions > 0 && ktx->numDimensions < 4);
    SDL_assert(ktx->generateMipmaps == false);

    image_info.flags = 0;
    image_info.imageType = static_cast<VkImageType>(ktx->numDimensions - 1);
    image_info.format = static_cast<VkFormat>(ktx2->vkFormat);
    image_info.extent.width = ktx->baseWidth;
    image_info.extent.height = ktx->baseHeight;
    image_info.extent.depth = ktx->baseDepth;
    image_info.mipLevels = ktx->numLevels;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ktx->isArray) {
        image_info.arrayLayers = ktx->numLayers;
        image_info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    } else {
        image_info.arrayLayers = 1;
    }
    if (ktx->isCubemap) {
        image_info.arrayLayers *= 6;
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    mem_createinfo.flags = 0;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_DEMAND(vmaCreateImage(r_allocator, &image_info, &mem_createinfo, &m_image, &m_image_mem, nullptr));
    image_view_info.image = m_image;
    image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_info.format = image_info.format;
    image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_view_info.subresourceRange.baseMipLevel = 0;
    image_view_info.subresourceRange.levelCount = image_info.mipLevels;
    image_view_info.subresourceRange.baseArrayLayer = 0;
    image_view_info.subresourceRange.layerCount = image_info.arrayLayers;
    VK_DEMAND(vkCreateImageView(r_device, &image_view_info, nullptr, &m_image_view));

    VkSamplerCreateInfo sampler_info {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_FALSE; // TODO these are user preferences
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_DEMAND(vkCreateSampler(r_device, &sampler_info, nullptr, &m_sampler));

    SDL_assert(ktxTexture_LoadImageData(ktx, staging_data + staging_offset, twogame::SceneHost::STAGING_BUFFER_SIZE - staging_offset) == KTX_SUCCESS);
    ktx_mip_iterate_userdata mip_data(image_info.arrayLayers, staging_offset);
    SDL_assert(ktxTexture_IterateLevels(ktx, ktx_mip_iterate, &mip_data) == KTX_SUCCESS);
    staging_offset += (ktxTexture_GetDataSizeUncompressed(ktx) + 15) & ~15;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = image_info.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = image_info.arrayLayers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(prepare_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(prepare_commands, staging_buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_data.regions().size(), mip_data.regions().data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (renderer->queue_family_index(twogame::QueueType::Graphics) != renderer->queue_family_index(twogame::QueueType::Transfer)) {
        barrier.srcQueueFamilyIndex = renderer->queue_family_index(twogame::QueueType::Transfer);
        barrier.dstQueueFamilyIndex = renderer->queue_family_index(twogame::QueueType::Graphics);
    }
    vkCmdPipelineBarrier(prepare_commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    // TODO many things:  - Use the pipeline barrier 2 interface
    // - Submit all image layout transitions batched together first, then do all buffer/image copies, then all QF/post barriers together after
    // but this is fine for now

    std::array<VkDescriptorBufferInfo, 2> descriptor_buffer_writes {};
    std::array<VkDescriptorImageInfo, 1> descriptor_image_writes {};
    std::array<VkWriteDescriptorSet, 4> descriptor_writes {};
    descriptor_buffer_writes[0].buffer = m_uniform_buffer[0];
    descriptor_buffer_writes[0].offset = 0;
    descriptor_buffer_writes[0].range = sizeof(glm::mat4);
    descriptor_buffer_writes[1].buffer = m_uniform_buffer[1];
    descriptor_buffer_writes[1].offset = 0;
    descriptor_buffer_writes[1].range = sizeof(glm::mat4);
    descriptor_image_writes[0].sampler = m_sampler;
    descriptor_image_writes[0].imageView = m_image_view;
    descriptor_image_writes[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[0].dstSet = m_descriptor_sets[0][0];
    descriptor_writes[0].dstBinding = 0;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[0].descriptorCount = 1;
    descriptor_writes[0].pBufferInfo = &descriptor_buffer_writes[0];
    descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[1].dstSet = m_descriptor_sets[0][1];
    descriptor_writes[1].dstBinding = 0;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_writes[1].descriptorCount = 1;
    descriptor_writes[1].pImageInfo = &descriptor_image_writes[0];
    descriptor_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[2].dstSet = m_descriptor_sets[1][0];
    descriptor_writes[2].dstBinding = 0;
    descriptor_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[2].descriptorCount = 1;
    descriptor_writes[2].pBufferInfo = &descriptor_buffer_writes[1];
    descriptor_writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[3].dstSet = m_descriptor_sets[1][1];
    descriptor_writes[3].dstBinding = 0;
    descriptor_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_writes[3].descriptorCount = 1;
    descriptor_writes[3].pImageInfo = &descriptor_image_writes[0];
    vkUpdateDescriptorSets(r_device, descriptor_writes.size(), descriptor_writes.data(), 0, nullptr);

    ktxTexture_Destroy(ktx);
    PHYSFS_close(duckdata);
    PHYSFS_close(imagedata);
    return true;
}

void DuckScene::handle_event(const SDL_Event& evt, twogame::SceneHost* stage)
{
}

void DuckScene::tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage)
{
}

void DuckScene::record_commands(twogame::IRenderer* renderer, uint32_t frame_number)
{
    vkResetCommandPool(r_device, m_draw_cmd_pool[frame_number % SIMULTANEOUS_FRAMES], 0);

    // proj should come from renderer
    float cot_vertical_fov = 1.0f / glm::tan(glm::radians(45.0) * 0.5);
    glm::mat4 proj(0.0f);
    proj[0][0] = cot_vertical_fov * renderer->swapchain_extent().height / renderer->swapchain_extent().width;
    proj[1][1] = -cot_vertical_fov;
    proj[2][2] = -1.0f;
    proj[2][3] = -1.0f;
    proj[3][2] = -0.1f;
    glm::mat4 view = glm::lookAt(glm::vec3(0, 250, -400), glm::vec3(0, 100, 0), glm::vec3(0, 1, 0));
    glm::mat4 proj_view = proj * view;

    void* mapped_buffer;
    vmaMapMemory(r_allocator, m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES], &mapped_buffer);
    memcpy(mapped_buffer, &proj_view, sizeof(glm::mat4));
    vmaUnmapMemory(r_allocator, m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES]);
    vmaFlushAllocation(r_allocator, m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES], 0, VK_WHOLE_SIZE);

    VkCommandBuffer cmd = m_draw_cmd[frame_number % SIMULTANEOUS_FRAMES][0];
    VkCommandBufferBeginInfo begin_info {};
    VkCommandBufferInheritanceInfo inherit_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inherit_info;
    inherit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inherit_info.renderPass = renderer->render_pass();
    inherit_info.subpass = 0;
    VK_DEMAND(vkBeginCommandBuffer(cmd, &begin_info));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline(0));

    VkExtent2D swapchain_extent = renderer->swapchain_extent();
    VkViewport viewport {};
    VkRect2D scissor {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = swapchain_extent.width;
    viewport.height = swapchain_extent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset = { 0, 0 };
    scissor.extent = swapchain_extent;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline_layout(0), 0, m_descriptor_sets[frame_number % SIMULTANEOUS_FRAMES].size(), m_descriptor_sets[frame_number % SIMULTANEOUS_FRAMES].data(), 0, nullptr);

    std::array<VkBuffer, 3> buffers = { m_buffer, m_buffer, m_buffer };
    std::array<VkDeviceSize, 3> buffer_offs = { 28788, 0, 57576 };
    std::array<float, 8> trs = { 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    vkCmdBindIndexBuffer(cmd, m_buffer, 76768, VK_INDEX_TYPE_UINT16);
    vkCmdBindVertexBuffers(cmd, 0, buffers.size(), buffers.data(), buffer_offs.data());
    vkCmdPushConstants(cmd, renderer->pipeline_layout(0), VK_SHADER_STAGE_VERTEX_BIT, 0, trs.size() * sizeof(decltype(trs)::value_type), trs.data());
    vkCmdDrawIndexed(cmd, 12636, 1, 0, 0, 0);
    vkEndCommandBuffer(cmd);
}

std::span<VkCommandBuffer> DuckScene::draw_commands(uint32_t frame_number, int subpass)
{
    auto& frame_commands = m_draw_cmd[frame_number % SIMULTANEOUS_FRAMES];
    switch (subpass) {
    case 0:
        return std::span(frame_commands).subspan(0, 1);
    default:
        std::abort();
    }
}

// fields that could be part of appstate
struct AppState {
    twogame::DisplayHost* host;
    twogame::SimpleForwardRenderer* renderer;
    twogame::SceneHost* stage;

    AppState()
        : host(nullptr)
        , renderer(nullptr)
        , stage(nullptr)
    {
    }
};

SDL_AppResult SDL_AppInit(void** _appstate, int argc, char** argv)
{
    static struct AppState appstate;
    *_appstate = &appstate;

    SDL_SetAppMetadata(APP_NAME, "0.0", "gh." SHORT_ORG_NAME "." SHORT_APP_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, ORG_NAME);
#ifdef DEBUG_BUILD
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif
    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_SENSOR;
    if (volkInitialize() != VK_SUCCESS) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "volkInitialize: no loader found");
        return SDL_APP_FAILURE;
    }
    if (SDL_Init(init_flags) == false) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

#ifdef DEBUG_BUILD
    constexpr const char* rsrc_root = TWOGAME_SOURCE_ROOT "/resources";
    constexpr const char* pref_root = TWOGAME_SOURCE_ROOT "/prefs";
    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(rsrc_root, "/data", 0) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /data/: %s", rsrc_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(pref_root, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /pref/: %s", pref_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    PHYSFS_setWriteDir(pref_root);
#else
    twogame::init_filesystem(argv[0], SHORT_ORG_NAME, SHORT_APP_NAME);
    char mountpoint[4096];
    const char* base_path = SDL_GetBasePath();

    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    for (const auto& dirent : std::filesystem::directory_iterator(base_path)) {
        if (dirent.is_regular_file() == false && dirent.is_directory() == false)
            continue;

        const auto& path = dirent.path();
        if (path.has_filename() && path.has_stem() && strncasecmp(path.extension().c_str(), ".pk2", 4) == 0) {
            const char* fullpath = path.c_str();
            snprintf(mountpoint, 4096, "/%s", path.stem().c_str());
            if (PHYSFS_mount(fullpath, mountpoint, 1) == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> %s/: %s", fullpath, mountpoint, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> %s/", fullpath, mountpoint);
            }
        }
    }

    char* pref_path = SDL_GetPrefPath(SHORT_ORG_NAME, SHORT_APP_NAME);
    if (PHYSFS_mount(pref_path, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> /pref/: %s", pref_path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> /pref/", pref_path);
    }
    PHYSFS_setWriteDir(pref_path);
    SDL_free(pref_path);
#endif

    try {
        appstate.host = new twogame::DisplayHost;
        appstate.renderer = new twogame::SimpleForwardRenderer(appstate.host);
        appstate.stage = new twogame::SceneHost(appstate.renderer, new DuckScene(appstate.host));
    } catch (...) {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (evt->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    appstate->stage->push_event(evt);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    appstate->stage->tick();
    appstate->host->draw_frame(appstate->renderer, appstate->stage);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (appstate->stage)
        delete appstate->stage;
    if (appstate->renderer)
        delete appstate->renderer;
    if (appstate->host)
        delete appstate->host;
    if (PHYSFS_isInit())
        PHYSFS_deinit();
    SDL_Quit();
}
