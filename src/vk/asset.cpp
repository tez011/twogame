#include <ktx.h>
#include <physfs.h>
#include "scene.h"

namespace twogame {

void IAsset::post_prepare(uint64_t ready)
{
    m_prepared = ready;
}

}

namespace twogame::asset {

namespace image {

    class ktx_mip_iterate_userdata {
    private:
        std::vector<VkBufferImageCopy2> m_regions;
        uint32_t m_layer_count;
        VkDeviceSize m_offset;

    public:
        ktx_mip_iterate_userdata(VkImageCreateInfo& image_info, VkDeviceSize offset)
            : m_layer_count(image_info.arrayLayers)
            , m_offset(offset)
        {
            m_regions.reserve(m_layer_count);
        }

        const std::vector<VkBufferImageCopy2>& regions() const { return m_regions; }

        void add_region(int miplevel, int face, int width, int height, int depth, ktx_uint64_t face_lod_size)
        {
            VkBufferImageCopy2& region = m_regions.emplace_back();
            SDL_assert(face == 0);
            region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
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

    struct prep {
        PHYSFS_File* fh;
        ktxTexture2* ktx2 = nullptr;

        prep(std::string_view path)
        {
            fh = PHYSFS_openRead(path.data());
            ktxStream kstream = ktx_physfs_istream(fh);
            ktx_error_code_e k_res = ktxTexture2_CreateFromStream(&kstream, 0, &ktx2);
            SDL_assert_release(k_res == KTX_SUCCESS);
            SDL_assert(ktx2->vkFormat);

            ktxTexture* ktx = reinterpret_cast<ktxTexture*>(ktx2);
            SDL_assert(ktx->numDimensions > 0 && ktx->numDimensions < 4);
            SDL_assert(ktx->generateMipmaps == false);

            if (ktxTexture2_NeedsTranscoding(ktx2)) {
                VkPhysicalDeviceFeatures device_features {};
                vkGetPhysicalDeviceFeatures(DisplayHost::hardware_device(), &device_features);

                khr_df_model_e color_model = ktxTexture2_GetColorModel_e(ktx2);
                ktx_transcode_fmt_e tf;
                if (color_model == KHR_DF_MODEL_UASTC && device_features.textureCompressionASTC_LDR)
                    tf = KTX_TTF_ASTC_4x4_RGBA;
                else if (color_model == KHR_DF_MODEL_ETC1S && device_features.textureCompressionETC2)
                    tf = KTX_TTF_ETC2_RGBA;
                else if (device_features.textureCompressionASTC_LDR)
                    tf = KTX_TTF_ASTC_4x4_RGBA;
                else if (device_features.textureCompressionETC2)
                    tf = KTX_TTF_ETC2_RGBA;
                else if (device_features.textureCompressionBC)
                    tf = KTX_TTF_BC7_RGBA;
                else
                    tf = KTX_TTF_RGBA32;

                k_res = ktxTexture2_TranscodeBasis(ktx2, tf, 0);
                SDL_assert_release(k_res == KTX_SUCCESS);
            }
        }

        ~prep()
        {
            ktxTexture2_Destroy(ktx2);
            PHYSFS_close(fh);
        }
    };

}

namespace mesh {

    struct prep {
        PHYSFS_File* fh;
        struct buffer {
            VkBuffer handle;
            VmaAllocation mem;
            VkMemoryPropertyFlags flags;
        } vertex_buffer, index_buffer;

        prep(std::string_view path)
        {
            fh = PHYSFS_openRead(path.data());

            VmaAllocationInfo alloc_info;
            VmaAllocationCreateInfo alloc_ci {};
            alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
            alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

            VkBufferCreateInfo buffer_ci {};
            buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_ci.size = 25272;
            buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &index_buffer.handle, &index_buffer.mem, &alloc_info));
            vmaGetMemoryTypeProperties(twogame::DisplayHost::allocator(), alloc_info.memoryType, &index_buffer.flags);

            buffer_ci.size = 76768;
            buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &vertex_buffer.handle, &vertex_buffer.mem, &alloc_info));
            vmaGetMemoryTypeProperties(twogame::DisplayHost::allocator(), alloc_info.memoryType, &vertex_buffer.flags);
        }
        ~prep()
        {
            PHYSFS_close(fh);
        }
    };

}

Image::Image()
    : m_image(VK_NULL_HANDLE)
    , m_mem(VK_NULL_HANDLE)
    , m_image_view(VK_NULL_HANDLE)
{
    m_prepared = std::make_shared<image::prep>("/data/duck.i0.ktx2");
}

Image::~Image()
{
    vkDestroyImageView(DisplayHost::device(), m_image_view, nullptr);
    vmaDestroyImage(DisplayHost::allocator(), m_image, m_mem);
}

size_t Image::prepare_needs() const
{
    auto p_prepare_data = std::get_if<std::shared_ptr<void>>(&m_prepared);
    if (p_prepare_data) {
        image::prep* prepare_data = static_cast<image::prep*>(p_prepare_data->get());
        ktxTexture* ktx = reinterpret_cast<ktxTexture*>(prepare_data->ktx2);
        return (ktxTexture_GetDataSizeUncompressed(ktx) + 15) & ~15;
    } else {
        return 0;
    }
}

size_t Image::prepare(SceneHost::StagingBuffer& commands, VkDeviceSize staging_offset)
{
    image::prep* prepare_data = static_cast<image::prep*>(std::get<std::shared_ptr<void>>(m_prepared).get());
    ktxTexture* ktx = reinterpret_cast<ktxTexture*>(prepare_data->ktx2);

    VmaAllocationCreateInfo alloc_info {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImageCreateInfo image_info {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = 0;
    image_info.imageType = static_cast<VkImageType>(ktx->numDimensions - 1);
    image_info.format = static_cast<VkFormat>(prepare_data->ktx2->vkFormat);
    image_info.extent.width = ktx->baseWidth;
    image_info.extent.height = ktx->baseHeight;
    image_info.extent.depth = ktx->baseDepth;
    image_info.mipLevels = ktx->numLevels;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageViewCreateInfo image_view_info {};
    image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    if (ktx->isArray && ktx->isCubemap) {
        image_info.arrayLayers = 6 * ktx->numLayers;
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        image_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    } else if (ktx->isCubemap) {
        image_info.arrayLayers = 6;
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        image_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (ktx->isArray) {
        image_info.arrayLayers = ktx->numLayers;
        if (ktx->numDimensions == 1) {
            image_view_info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        } else if (ktx->numDimensions == 2) {
            image_info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
            image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        }
    } else {
        image_info.arrayLayers = 1;
        image_view_info.viewType = static_cast<VkImageViewType>(image_info.imageType);
    }
    VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &image_info, &alloc_info, &m_image, &m_mem, nullptr));

    image_view_info.image = m_image;
    image_view_info.format = image_info.format;
    image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_view_info.subresourceRange.baseMipLevel = 0;
    image_view_info.subresourceRange.levelCount = image_info.mipLevels;
    image_view_info.subresourceRange.baseArrayLayer = 0;
    image_view_info.subresourceRange.layerCount = image_info.arrayLayers;
    VK_DEMAND(vkCreateImageView(DisplayHost::device(), &image_view_info, nullptr, &m_image_view));

    std::span<std::byte> staging_data = commands.window(staging_offset);
    image::ktx_mip_iterate_userdata mip_data(image_info, staging_offset);
    ktx_error_code_e res = ktxTexture_LoadImageData(ktx, reinterpret_cast<ktx_uint8_t*>(staging_data.data()), staging_data.size());
    SDL_assert_release(res == KTX_SUCCESS);
    res = ktxTexture_IterateLevels(ktx, image::ktx_mip_iterate, &mip_data);
    SDL_assert_release(res == KTX_SUCCESS);
    commands.copy_image(m_image, image_info, mip_data.regions(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (ktxTexture_GetDataSizeUncompressed(ktx) + 15) & ~15;
}

Material::Material()
{
    m_base_color_texture = std::make_shared<Image>();
}

Material::~Material()
{
}

void Material::push_dependents(std::queue<IAsset*>& deps) const
{
    deps.push(m_base_color_texture.get());
}

size_t Material::prepare_needs() const
{
    return 0;
}

size_t Material::prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset)
{
    return 0;
}

Mesh::Mesh()
{
    auto prep = std::make_shared<mesh::prep>("/data/duck.bin");
    m_prepared = prep;
    m_materials.emplace_back(new Material);

    m_vertex_buffer = prep->vertex_buffer.handle;
    m_vertex_mem = prep->vertex_buffer.mem;
    m_index_buffer = prep->index_buffer.handle;
    m_index_mem = prep->index_buffer.mem;
}

Mesh::~Mesh()
{
    vmaDestroyBuffer(DisplayHost::allocator(), m_vertex_buffer, m_vertex_mem);
    vmaDestroyBuffer(DisplayHost::allocator(), m_index_buffer, m_index_mem);
}

void Mesh::push_dependents(std::queue<IAsset*>& deps) const
{
    for (auto it = m_materials.cbegin(); it != m_materials.cend(); ++it)
        deps.push(it->get());
}

size_t Mesh::prepare_needs() const
{
    size_t needs = 0;
    auto p_prepare_data = std::get_if<std::shared_ptr<void>>(&m_prepared);
    if (p_prepare_data) {
        mesh::prep* prepare_data = static_cast<mesh::prep*>(p_prepare_data->get());
        if ((prepare_data->vertex_buffer.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            needs += 76768;
        if (prepare_data->index_buffer.handle && (prepare_data->index_buffer.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            needs += 25272;
    }
    return needs;
}

size_t Mesh::prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset)
{
    mesh::prep* prep = static_cast<mesh::prep*>(std::get<std::shared_ptr<void>>(m_prepared).get());
    size_t staged_size = 0;
    if (prep->vertex_buffer.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* vertex_buffer_ptr;
        VK_DEMAND(vmaMapMemory(DisplayHost::allocator(), m_vertex_mem, &vertex_buffer_ptr));
        PHYSFS_seek(prep->fh, 0);
        PHYSFS_readBytes(prep->fh, vertex_buffer_ptr, 76768);
        vmaUnmapMemory(DisplayHost::allocator(), m_vertex_mem);
        if ((prep->vertex_buffer.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            vmaFlushAllocation(DisplayHost::allocator(), m_vertex_mem, 0, VK_WHOLE_SIZE);
    } else {
        VkBufferCopy2 copy {};
        copy.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        copy.srcOffset = offset;
        copy.dstOffset = 0;
        copy.size = 76768;

        PHYSFS_seek(prep->fh, 0);
        PHYSFS_readBytes(prep->fh, commands.window(offset).data(), 76768);
        commands.copy_buffer(m_vertex_buffer, 76768, std::span(&copy, 1), VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        staged_size += 76768;
    }
    if (prep->index_buffer.handle && (prep->index_buffer.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        void* index_buffer_ptr;
        VK_DEMAND(vmaMapMemory(DisplayHost::allocator(), m_index_mem, &index_buffer_ptr));
        PHYSFS_seek(prep->fh, 76768);
        PHYSFS_readBytes(prep->fh, index_buffer_ptr, 25272);
        vmaUnmapMemory(DisplayHost::allocator(), m_index_mem);
        if ((prep->index_buffer.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            vmaFlushAllocation(DisplayHost::allocator(), m_index_mem, 0, VK_WHOLE_SIZE);
    } else {
        VkBufferCopy2 copy {};
        copy.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        copy.srcOffset = offset;
        copy.dstOffset = 0;
        copy.size = 25272;

        PHYSFS_seek(prep->fh, 76768);
        PHYSFS_readBytes(prep->fh, commands.window(offset).data(), 25272);
        commands.copy_buffer(m_vertex_buffer, 25272, std::span(&copy, 1), VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        staged_size += 25272;
    }
    return staged_size;
}

}
