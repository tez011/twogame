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

            SDL_assert(ktx2->vkFormat);
        }

        ~prep()
        {
            ktxTexture2_Destroy(ktx2);
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

size_t Image::prepare(IScene* scene, StagingBuffer& commands)
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

    std::byte* staging_data = commands.tail();
    image::ktx_mip_iterate_userdata mip_data(image_info, commands.tail_offset());
    ktx_error_code_e res = ktxTexture_IterateLevels(ktx, image::ktx_mip_iterate, &mip_data);
    SDL_assert_release(res == KTX_SUCCESS);
    commands.copy_image(m_image, image_info, mip_data.regions(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (ktx->pData) {
        // if the image was transcoded, copy it out of the decoded data
        memcpy(staging_data, ktx->pData, ktx->dataSize);
        return (ktx->dataSize + 15) & ~15;
    } else {
        res = ktxTexture_LoadImageData(ktx, reinterpret_cast<ktx_uint8_t*>(staging_data), SceneHost::STAGING_BUFFER_SIZE - commands.tail_offset());
        SDL_assert_release(res == KTX_SUCCESS);
        return (ktxTexture_GetDataSizeUncompressed(ktx) + 15) & ~15;
    }
}

Mesh::Mesh()
    : m_pipeline_key(0)
{
    m_vertex_count = 2399;
    m_index_count = 12636;
    m_uv_channels = 1;
    m_color_channels = 1;
    m_32bit_indexes = 0;
}

Mesh::~Mesh()
{
}

VkDeviceAddress Mesh::vertices_offset() const
{
    size_t indexes_size = m_index_count * (m_32bit_indexes ? sizeof(uint32_t) : sizeof(uint16_t));
    return (indexes_size + 15) & ~15;
}

VkDeviceAddress Mesh::normals_offset() const
{
    size_t indexes_size = vertices_offset();
    size_t vertices_size = m_vertex_count * (1 + (m_uv_channels + 1) / 2) * 4 * sizeof(float);
    return indexes_size + vertices_size;
}

size_t Mesh::prepare_needs() const
{
    size_t normals_size = m_vertex_count * (2 + m_color_channels) * 4 * sizeof(float);
    return normals_offset() + normals_size;
}

size_t Mesh::prepare(IScene* scene, StagingBuffer& commands)
{
    std::vector<std::byte> mesh_data(prepare_needs());
    PHYSFS_File* fh = PHYSFS_openRead("/data/duck.bin");
    PHYSFS_readBytes(fh, mesh_data.data(), mesh_data.size());
    PHYSFS_close(fh);

    return scene->prepare_mesh(this, mesh_data, commands);
}

}
