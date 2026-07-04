#include "asset.h"
#include <numeric>
#include <ktx.h>
#include <SDL3/SDL.h>
#include "asset.fbs.hpp"
#include "display.h"
#include "scene.h"

namespace twogame {

mat4s TRS::transform_matrix() const
{
    mat4s t = glms_translate_make(translation),
          r = glms_quat_mat4(rotation),
          s = glms_scale_make(scale);
    return glms_mat4_mul(t, glms_mat4_mul(r, s));
}

void IAsset::post_prepare(uint64_t ready)
{
    m_prepared = ready;
}

}

namespace twogame::asset {

void Animation::Sampler::interpolate(float t, vec4* output, uint32_t* hint, bool force_step) const
{
    uint32_t keyframe = std::numeric_limits<uint32_t>::max(), total_targets = targets();
    if (t < timeline.front()) {
        keyframe = 0;
        force_step = true;
    } else if (t >= timeline.back()) {
        keyframe = static_cast<uint32_t>(timeline.size() - 1);
        force_step = true;
    } else if (hint) {
        uint32_t starting_hint = std::clamp(*hint, 0U, static_cast<uint32_t>(timeline.size() - 1)),
                 max_search = std::min(starting_hint + 6, static_cast<uint32_t>(timeline.size()));
        for (uint32_t hv = starting_hint; hv < max_search; hv++) {
            if (timeline[hv] <= t && (hv + 1 == timeline.size() || t < timeline[hv + 1])) {
                keyframe = hv;
                break;
            }
        }
    }
    if (keyframe == std::numeric_limits<uint32_t>::max()) {
        auto it = std::upper_bound(timeline.begin(), timeline.end(), t);
        if (it == timeline.begin())
            keyframe = 0;
        else
            keyframe = (it - timeline.begin()) - 1;
    }
    if (hint)
        *hint = keyframe;

    float pct = glm_percentc(timeline[keyframe], timeline[keyframe + 1], t);
    if (force_step) {
        memcpy(output, &channels[keyframe * total_targets], total_targets * sizeof(vec4));
    } else {
        memcpy(output, &channels[keyframe * total_targets], step_targets * sizeof(vec4));
        for (int i = 0; i < lerp_targets; i++)
            glm_vec4_lerp(const_cast<float*>(channels[keyframe * total_targets + step_targets + i].raw),
                const_cast<float*>(channels[(keyframe + 1) * total_targets + step_targets + i].raw),
                pct, reinterpret_cast<float*>(output + step_targets + i));
        for (int i = 0; i < slerp_targets; i++)
            glm_quat_slerp(const_cast<float*>(channels[keyframe * total_targets + step_targets + lerp_targets + i].raw),
                const_cast<float*>(channels[(keyframe + 1) * total_targets + step_targets + lerp_targets + i].raw),
                pct, reinterpret_cast<float*>(output + step_targets + lerp_targets + i));
    }
}

Animation::Animation(const SceneManifest& source, size_t source_index, size_t dst_index)
    : m_keyframe_width(0)
    , m_duration(0)
{
    const fbs::Animation* info = source.manifest()->animations()->Get(source_index);
    m_samplers.reserve(info->samplers()->size());
    m_targets.reserve(info->targets()->size());
#ifdef DEBUG_BUILD
    size_t keyframe_width_by_sampler = 0;
#endif

    for (auto it = info->samplers()->begin(); it != info->samplers()->end(); ++it) {
        Sampler& sampler = m_samplers.emplace_back();
        sampler.timeline = source.buffer<float>(it->timeline());
        sampler.channels = source.buffer<vec4s>(it->channels());
        sampler.lerp_targets = it->lerp_targets();
        sampler.step_targets = it->step_targets();
        sampler.slerp_targets = it->slerp_targets();
        m_duration = std::max(m_duration, sampler.timeline.back());
#ifdef DEBUG_BUILD
        keyframe_width_by_sampler += sampler.lerp_targets + sampler.step_targets + sampler.slerp_targets;
#endif
    }
    for (auto it = info->targets()->begin(); it != info->targets()->end(); ++it) {
        AnimationTarget& target = m_targets.emplace_back();
        target.object = it->object();
        target.width = it->width();
        target.field = static_cast<unsigned>(it->field());
        target.object_is_bone = it->object_is_bone();
        m_keyframe_width += target.width;
    }
#ifdef DEBUG_BUILD
    SDL_assert(keyframe_width_by_sampler == m_keyframe_width);
#endif
}

void Animation::interpolate(float t, vec4* dest, std::span<uint32_t> hints, bool force_step) const
{
    size_t tcs = 0;
    for (size_t i = 0; i < m_samplers.size(); i++) {
        m_samplers[i].interpolate(t, dest + tcs, hints.empty() ? nullptr : &hints[i], force_step);
        tcs += m_samplers[i].targets();
    }
}

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

    static ktx_error_code_e ktx_mip_iterate(int miplevel, int face, int width, int height, int depth, ktx_uint64_t face_lod_size, void* pixels, void* userdata)
    {
        ktx_mip_iterate_userdata* mip_data = reinterpret_cast<ktx_mip_iterate_userdata*>(userdata);
        mip_data->add_region(miplevel, face, width, height, depth, face_lod_size);
        (void)(pixels);
        return KTX_SUCCESS;
    }

    struct prep {
        std::vector<std::byte> fh_data;
        ktxTexture2* ktx2 = nullptr;

        prep(const SceneManifest& asset_source, size_t image_index)
        {
            fh_data = asset_source.buffer(asset_source.manifest()->images()->Get(image_index));
            ktx_error_code_e k_res = ktxTexture2_CreateFromMemory(reinterpret_cast<ktx_uint8_t*>(fh_data.data()), fh_data.size(), 0, &ktx2);
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
        }
    };

}

Image::Image(const SceneManifest& source, size_t source_index, size_t dst_index)
    : m_image(VK_NULL_HANDLE)
    , m_mem(VK_NULL_HANDLE)
    , m_image_view(VK_NULL_HANDLE)
{
    m_prepared = std::make_shared<image::prep>(source, source_index);
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
    auto prepare_data = std::static_pointer_cast<image::prep>(std::get<std::shared_ptr<void>>(m_prepared));
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
        res = ktxTexture_LoadImageData(ktx, reinterpret_cast<ktx_uint8_t*>(staging_data), commands.size() - commands.tail_offset());
        SDL_assert_release(res == KTX_SUCCESS);
        return (ktxTexture_GetDataSizeUncompressed(ktx) + 15) & ~15;
    }
}

namespace mesh {

    enum SubBufferID {
        SubBuffer_Index,
        SubBuffer_Position,
        SubBuffer_Normal,
        SubBuffer_Joints,
        SubBuffer_DPosition,
        SubBuffer_DNormal,
        SubBuffer_MAX_VALUE,
    };

    struct prep {
        SceneManifest::BufferResolver buffer_resolver;
        std::array<size_t, SubBuffer_MAX_VALUE> buffers, buffer_sizes;
        size_t mesh_index;

        prep(const SceneManifest& source, size_t source_index, size_t dst_index)
            : buffer_resolver(source.buffer_resolver())
            , mesh_index(dst_index)
        {
            const fbs::Assets* manifest = source.manifest().get();
            const fbs::Mesh* info = manifest->meshes()->Get(source_index);
            buffers[SubBuffer_Index] = info->indexes();
            buffers[SubBuffer_Position] = info->positions();
            buffers[SubBuffer_Normal] = info->normals();
            buffers[SubBuffer_Joints] = info->joints();
            buffers[SubBuffer_DPosition] = info->position_displacements();
            buffers[SubBuffer_DNormal] = info->normal_displacements();
            std::transform(buffers.begin(), buffers.end(), buffer_sizes.begin(), [manifest](size_t buffer_index) {
                return buffer_index ? manifest->buffers()->Get(buffer_index - 1) : 0;
            });
        }
        ~prep() { }
    };

}

Mesh::Mesh(const SceneManifest& source, size_t source_index, size_t dst_index)
{
    std::shared_ptr<mesh::prep> prepare_data = std::make_shared<mesh::prep>(source, source_index, dst_index);
    const fbs::Mesh* info = source.manifest()->meshes()->Get(source_index);
    m_vertex_count = info->vertex_count();
    m_index_count = info->index_count();
    m_displacement_count = info->displace_weights() ? info->displace_weights()->size() : 0;

    m_uv_channels = 2 * (prepare_data->buffer_sizes[mesh::SubBuffer_Position] / (m_vertex_count * sizeof(vec4))) - 2;
    m_color_channels = prepare_data->buffer_sizes[mesh::SubBuffer_Normal] / (m_vertex_count * sizeof(vec4)) - 2;
    m_joint_count = prepare_data->buffer_sizes[mesh::SubBuffer_Joints] / ((sizeof(vec4) + sizeof(ivec4)) * m_vertex_count);
    switch (prepare_data->buffer_sizes[mesh::SubBuffer_Index] / m_index_count) {
    case 2:
        m_32bit_indexes = false;
        break;
    case 4:
        m_32bit_indexes = true;
        break;
    default:
        std::abort();
    }

    m_prepared = prepare_data;
}

Mesh::~Mesh()
{
}

size_t Mesh::prepare_needs() const
{
    auto p_prepare_data = std::get_if<std::shared_ptr<void>>(&m_prepared);
    if (p_prepare_data) {
        mesh::prep* prepare_data = static_cast<mesh::prep*>(p_prepare_data->get());
        size_t needs = 0;
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++)
            needs += (prepare_data->buffer_sizes[i] + 15) & (~15);
        return needs;
    } else {
        return 0;
    }
}

size_t Mesh::prepare(IScene* scene, StagingBuffer& commands)
{
    mesh::prep* prepare_data = static_cast<mesh::prep*>(std::get<std::shared_ptr<void>>(m_prepared).get());
    VkBufferDeviceAddressInfo bda_info {};
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = scene->mesh_data_buffer();

    std::span<std::byte> mesh_data = scene->mesh_data_pointer();
    std::array<std::vector<std::byte>, mesh::SubBuffer_MAX_VALUE> sub_buffers;
    VkDeviceAddress base_vertices_addr = vkGetBufferDeviceAddress(DisplayHost::device(), &bda_info), base_offset = scene->mesh_references()[prepare_data->mesh_index].index_buffer_address - base_vertices_addr;
    for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++)
        sub_buffers[i] = prepare_data->buffer_resolver(prepare_data->buffers[i]);

    VkDeviceAddress delta = 0;
    if (mesh_data.empty()) {
        std::vector<VkBufferCopy2> copies;
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++) {
            if (sub_buffers[i].empty())
                continue;

            copies.emplace_back().sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
            copies.back().srcOffset = commands.tail_offset() + delta;
            copies.back().dstOffset = base_offset + delta;
            copies.back().size = sub_buffers[i].size();
            memcpy(commands.tail() + delta, sub_buffers[i].data(), sub_buffers[i].size());
            delta += (sub_buffers[i].size() + 15) & ~15;
        }
        commands.copy_buffer(scene->mesh_data_buffer(), copies, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        return delta;
    } else {
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++) {
            memcpy(mesh_data.subspan(base_offset + delta).data(), sub_buffers[i].data(), sub_buffers[i].size());
            delta += (sub_buffers[i].size() + 15) & ~15;
        }
        return 0;
    }
}

size_t Mesh::write_buffer_addresses(std::span<MeshEntry> mesh_entries, VkDeviceAddress base_vertices_addr) const
{
    size_t delta = 0;
    auto prepare_data = std::static_pointer_cast<mesh::prep>(std::get<std::shared_ptr<void>>(m_prepared));
    MeshEntry& e = mesh_entries[prepare_data->mesh_index];
    e.index_buffer_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_Index] + 15) & ~15;
    e.vertex_buffer_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_Position] + 15) & ~15;
    e.normal_buffer_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_Normal] + 15) & ~15;
    e.joints_buffer_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_Joints] + 15) & ~15;
    e.vertex_displacement_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_DPosition] + 15) & ~15;
    e.normal_displacement_address = base_vertices_addr + delta;
    delta += (prepare_data->buffer_sizes[mesh::SubBuffer_DNormal] + 15) & ~15;
    e.joint_count = m_joint_count;
    e.displacement_count = m_displacement_count;
    return delta;
}

Skeleton::Skeleton(const SceneManifest& source, size_t source_index, size_t dst_index)
{
    const fbs::Skeleton* info = source.manifest()->skeletons()->Get(source_index);
    m_skin_matrices = source.buffer<mat4s>(info->skin_matrices());

    m_bone_parents.resize(info->nodes()->size(), std::numeric_limits<uint32_t>::max());
    m_bone_transforms.resize(info->nodes()->size(), TRS());
    for (size_t i = 0; i < info->nodes()->size(); i++) {
        const fbs::BoneNode* bnode = info->nodes()->Get(i);
        for (auto it = bnode->children()->begin(); bnode->children() && it != bnode->children()->end(); ++it) {
            SDL_assert(i < *it); // require that parents are ordered before children
            m_bone_parents[*it] = i;
        }

        if (bnode->transform_type() == fbs::Transform::Mat4) {
            mat4s& xfm = m_bone_transforms[i].emplace<mat4s>();
            memcpy(xfm.raw, bnode->transform_as_Mat4(), sizeof(mat4s));
        } else if (bnode->transform_type() == fbs::Transform::TRS) {
            TRS& trs = m_bone_transforms[i].emplace<TRS>();
            memcpy(trs.translation.raw, bnode->transform_as_TRS()->translation().v(), sizeof(vec3));
            memcpy(trs.rotation.raw, bnode->transform_as_TRS()->rotation().v(), sizeof(versor));
            memcpy(trs.scale.raw, bnode->transform_as_TRS()->scale().v(), sizeof(vec3));
        }
    }

    m_joints.reserve(info->joints()->size());
    std::copy(info->joints()->begin(), info->joints()->end(), std::back_inserter(m_joints));
}

}
