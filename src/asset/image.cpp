#include <ktx.h>
#include <physfs.h>
#include "asset.h"
#include "render.h"
#include "xml.h"

struct ktx_mip_iterate_userdata {
    std::vector<VkBufferImageCopy>& regions;
    VkDeviceSize offset;
    uint32_t layer_count;

    ktx_mip_iterate_userdata(std::vector<VkBufferImageCopy>& regions, uint32_t layer_count)
        : regions(regions)
        , offset(0)
        , layer_count(layer_count)
    {
    }
};

struct ktx_physfs_istream : public ktxStream {
    static ktx_error_code_e _read(ktxStream* str, void* dst, const ktx_size_t count);
    static ktx_error_code_e _skip(ktxStream* str, const ktx_size_t count);
    static ktx_error_code_e _getpos(ktxStream* str, ktx_off_t* const offset);
    static ktx_error_code_e _setpos(ktxStream* str, const ktx_off_t offset);
    static ktx_error_code_e _getsize(ktxStream* str, ktx_size_t* const size);
    static void _destruct(ktxStream* str);

    ktx_physfs_istream(PHYSFS_File* fh)
    {
        type = eStreamTypeCustom;
        read = _read;
        skip = _skip;
        write = nullptr;
        getpos = _getpos;
        setpos = _setpos;
        getsize = _getsize;
        destruct = _destruct;
        data.custom_ptr.address = fh;
        closeOnDestruct = KTX_TRUE;
    }
    ktx_physfs_istream(ktx_physfs_istream&) = delete;
};

ktx_error_code_e ktx_physfs_istream::_read(ktxStream* self, void* dst, const ktx_size_t count)
{
    PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
    PHYSFS_sint64 rs = PHYSFS_readBytes(fh, dst, count);
    if (rs < 0)
        return KTX_FILE_READ_ERROR;
    else if (static_cast<ktx_size_t>(rs) < count && PHYSFS_eof(fh))
        return KTX_FILE_UNEXPECTED_EOF;
    else
        return KTX_SUCCESS;
}

ktx_error_code_e ktx_physfs_istream::_skip(ktxStream* self, const ktx_size_t count)
{
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
}

ktx_error_code_e ktx_physfs_istream::_getpos(ktxStream* self, ktx_off_t* const offset)
{
    if (offset) {
        PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
        PHYSFS_sint64 tell = PHYSFS_tell(fh);
        if (tell == -1)
            return KTX_FILE_SEEK_ERROR;
        *offset = tell;
    }
    return KTX_SUCCESS;
}

ktx_error_code_e ktx_physfs_istream::_setpos(ktxStream* self, const ktx_off_t offset)
{
    PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
    if (PHYSFS_seek(fh, offset))
        return KTX_SUCCESS;
    if (PHYSFS_getLastErrorCode() == PHYSFS_ERR_PAST_EOF)
        return KTX_FILE_UNEXPECTED_EOF;
    else
        return KTX_FILE_SEEK_ERROR;
}

ktx_error_code_e ktx_physfs_istream::_getsize(ktxStream* self, ktx_size_t* const size)
{
    PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
    PHYSFS_sint64 sz = PHYSFS_fileLength(fh);
    if (sz == -1)
        return KTX_FILE_DATA_ERROR;
    if (size)
        *size = sz;
    return KTX_SUCCESS;
}

void ktx_physfs_istream::_destruct(ktxStream* self)
{
    PHYSFS_File* fh = reinterpret_cast<PHYSFS_File*>(self->data.custom_ptr.address);
    PHYSFS_close(fh);
}

static ktx_error_code_e ktx_mip_iterate(int miplevel, int face, int width, int height, int depth, ktx_uint64_t face_lod_size, void* pixels, void* userdata)
{
    ktx_mip_iterate_userdata* ud = reinterpret_cast<ktx_mip_iterate_userdata*>(userdata);
    (void)(pixels);

    VkBufferImageCopy& region = ud->regions.emplace_back();
    region.bufferOffset = ud->offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = miplevel;
    region.imageSubresource.baseArrayLayer = face;
    region.imageSubresource.layerCount = ud->layer_count;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = depth;
    ud->offset += face_lod_size;
    return KTX_SUCCESS;
}

namespace twogame::asset {

Image::Image(const xml::assets::Image& info, const Renderer* r)
    : AbstractAsset(r)
{
    PHYSFS_File* fh = PHYSFS_openRead(info.source().data());
    if (!fh)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    ktxTexture2* ktx2 = nullptr;
    ktx_physfs_istream kstream(fh);
    ktx_error_code_e k_res = ktxTexture2_CreateFromStream(&kstream, 0, &ktx2);
    if (k_res != KTX_SUCCESS)
        throw MalformedException(info.name(), k_res == KTX_UNKNOWN_FILE_FORMAT ? "not a ktx2 image" : "failed to load ktx2 image");
    if (ktx2->vkFormat == 0)
        throw MalformedException(info.name(), "image data format not known");

    ktxTexture* ktx = reinterpret_cast<ktxTexture*>(ktx2);
    if (ktx->numDimensions == 0 || ktx->numDimensions > 3)
        throw MalformedException(info.name(), "invalid number of dimensions");
    if (ktx->generateMipmaps)
        throw MalformedException(info.name(), "create your mipmaps ahead of time!");
    if (ktx->isArray && ktx->numDimensions == 3)
        throw MalformedException(info.name(), "3D image arrays are not supported");

    VmaAllocationCreateInfo a_createinfo {};
    VkBufferCreateInfo b_createinfo {};
    VkImageCreateInfo i_createinfo {};
    b_createinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    b_createinfo.size = ktxTexture_GetDataSizeUncompressed(ktx);
    b_createinfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    b_createinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i_createinfo.flags = 0;
    i_createinfo.imageType = static_cast<VkImageType>(ktx->numDimensions - 1);
    i_createinfo.format = static_cast<VkFormat>(ktx2->vkFormat);
    i_createinfo.extent.width = ktx->baseWidth;
    i_createinfo.extent.height = ktx->baseHeight;
    i_createinfo.extent.depth = ktx->baseDepth;
    i_createinfo.mipLevels = ktx->numLevels;
    i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
    i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    i_createinfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    i_createinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ktx->isArray) {
        i_createinfo.arrayLayers = ktx->numLayers;
        i_createinfo.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    } else {
        i_createinfo.arrayLayers = 1;
    }
    if (ktx->isCubemap) {
        i_createinfo.arrayLayers *= 6;
        i_createinfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    m_mip_levels = i_createinfo.mipLevels;
    m_array_layers = i_createinfo.arrayLayers;

    VkImageFormatProperties ifmt;
    if (vkGetPhysicalDeviceImageFormatProperties(m_renderer.hwd(), i_createinfo.format, i_createinfo.imageType, i_createinfo.tiling, i_createinfo.usage, i_createinfo.flags, &ifmt) == VK_ERROR_FORMAT_NOT_SUPPORTED)
        throw MalformedException(info.name(), "image format not supported by hardware");

    VmaAllocationInfo storage_allocinfo;
    a_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    a_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &b_createinfo, &a_createinfo, &m_storage, &m_storage_mem, &storage_allocinfo));
    if ((k_res = ktxTexture_LoadImageData(ktx, static_cast<ktx_uint8_t*>(storage_allocinfo.pMappedData), b_createinfo.size)) != KTX_SUCCESS)
        throw MalformedException(info.name(), "failed to load image data");

    ktx_mip_iterate_userdata mip_ud(m_copies, m_array_layers);
    m_copies.reserve(m_mip_levels);
    if ((k_res = ktxTexture_IterateLevels(ktx, ktx_mip_iterate, &mip_ud)) != KTX_SUCCESS)
        throw MalformedException(info.name(), "failed to iterate texture levels");

    a_createinfo.usage = VMA_MEMORY_USAGE_AUTO;
    a_createinfo.flags = 0;
    VK_CHECK(vmaCreateImage(m_renderer.allocator(), &i_createinfo, &a_createinfo, &m_image, &m_image_mem, nullptr));

    VkImageViewCreateInfo v_createinfo {};
    v_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v_createinfo.image = m_image;
    v_createinfo.format = i_createinfo.format;
    v_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v_createinfo.subresourceRange.baseMipLevel = 0;
    v_createinfo.subresourceRange.levelCount = i_createinfo.mipLevels;
    v_createinfo.subresourceRange.baseArrayLayer = 0;
    v_createinfo.subresourceRange.layerCount = i_createinfo.arrayLayers;
    if (ktx->isArray && ktx->isCubemap)
        v_createinfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    else if (ktx->isCubemap)
        v_createinfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    else if (ktx->isArray)
        v_createinfo.viewType = static_cast<VkImageViewType>(ktx->numDimensions + VK_IMAGE_VIEW_TYPE_CUBE);
    else
        v_createinfo.viewType = static_cast<VkImageViewType>(ktx->numDimensions - 1);
    VK_CHECK(vkCreateImageView(m_renderer.device(), &v_createinfo, nullptr, &m_view));

    ktxTexture_Destroy(ktx);
}

Image::Image(Image&& other) noexcept
    : AbstractAsset(&other.m_renderer)
    , m_storage(other.m_storage)
    , m_image(other.m_image)
    , m_view(other.m_view)
    , m_storage_mem(other.m_storage_mem)
    , m_image_mem(other.m_image_mem)
    , m_mip_levels(other.m_mip_levels)
    , m_array_layers(other.m_array_layers)
    , m_copies(std::move(other.m_copies))
{
    other.m_storage = VK_NULL_HANDLE;
    other.m_image = VK_NULL_HANDLE;
    other.m_view = VK_NULL_HANDLE;
    other.m_storage_mem = VK_NULL_HANDLE;
    other.m_image_mem = VK_NULL_HANDLE;
}

Image::~Image()
{
    if (m_storage != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_renderer.allocator(), m_storage, m_storage_mem);
    if (m_view != VK_NULL_HANDLE)
        vkDestroyImageView(m_renderer.device(), m_view, nullptr);
    if (m_image != VK_NULL_HANDLE)
        vmaDestroyImage(m_renderer.allocator(), m_image, m_image_mem);
    m_storage = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
}

void Image::prepare(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = m_array_layers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(cmd, m_storage, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, m_copies.size(), m_copies.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Image::post_prepare()
{
    vmaDestroyBuffer(m_renderer.allocator(), m_storage, m_storage_mem);
    m_storage = VK_NULL_HANDLE;
    m_copies.clear();
}

bool Image::prepared() const
{
    return m_storage == VK_NULL_HANDLE;
}

}
