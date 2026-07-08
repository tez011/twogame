#include <algorithm>
#include <map>
#include <numeric>
#include <vector>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <mikktspace.h>

using namespace fastgltf::math;

namespace fastgltf {

std::pair<StaticVector<std::byte>, ComponentType> writeIndices(PrimitiveType type, std::size_t indexCount, std::size_t primitiveCount);

}

void generate_mesh_normals(fastgltf::Asset& asset, fastgltf::Primitive& mesh)
{
    auto& acc_positions = asset.accessors[mesh.findAttribute("POSITION")->accessorIndex];
    std::vector<fvec3> positions(acc_positions.count);
    std::vector<uint32_t> positions_sourcemap;
    fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset, acc_positions, positions.data());

    if (mesh.indicesAccessor) {
        auto& acc_indexes = asset.accessors[mesh.indicesAccessor.value()];
        std::vector<uint32_t> indexes(acc_indexes.count);
        if (acc_indexes.componentType == fastgltf::ComponentType::UnsignedByte) {
            fastgltf::iterateAccessorWithIndex<uint8_t>(asset, acc_indexes, [&indexes](uint8_t value, size_t index) {
                indexes[index] = value;
            });
        } else if (acc_indexes.componentType == fastgltf::ComponentType::UnsignedShort) {
            fastgltf::iterateAccessorWithIndex<uint16_t>(asset, acc_indexes, [&indexes](uint16_t value, size_t index) {
                indexes[index] = value;
            });
        } else if (acc_indexes.componentType == fastgltf::ComponentType::UnsignedInt) {
            fastgltf::copyFromAccessor<uint32_t>(asset, acc_indexes, indexes.data());
        } else {
            std::unreachable();
        }

        std::vector<fvec3> indexed_positions(indexes.size());
        indexed_positions.swap(positions);
        for (size_t i = 0; i < indexes.size(); i++)
            positions[i] = indexed_positions[indexes[i]];
        positions_sourcemap = std::move(indexes);
    } else {
        positions_sourcemap.resize(acc_positions.count);
        std::iota(positions_sourcemap.begin(), positions_sourcemap.end(), 0);
    }

    // Convert to Triangles order. We cannot really have any vertex sharing if we are generating face normals.
    // This may require a new mapping of new positions to old positions.
    if (mesh.type != fastgltf::PrimitiveType::Triangles) {
        std::vector<uint32_t> old_positions_sourcemap;
        std::vector<fvec3> expanded_positions;
        old_positions_sourcemap.swap(positions_sourcemap);
        expanded_positions.reserve(3 * positions.size());
        positions_sourcemap.reserve(3 * positions.size());
        for (size_t i = 0; i < positions.size() - 2; i++) {
            if (mesh.type == fastgltf::PrimitiveType::TriangleStrip) {
                expanded_positions.push_back(positions[i]);
                expanded_positions.push_back(positions[i + (1 + i % 2)]);
                expanded_positions.push_back(positions[i + (2 - i % 2)]);
                positions_sourcemap.push_back(old_positions_sourcemap[i]);
                positions_sourcemap.push_back(old_positions_sourcemap[i + (1 + i % 2)]);
                positions_sourcemap.push_back(old_positions_sourcemap[i + (2 - i % 2)]);
            } else if (mesh.type == fastgltf::PrimitiveType::TriangleFan) {
                expanded_positions.push_back(positions[i + 1]);
                expanded_positions.push_back(positions[i + 2]);
                expanded_positions.push_back(positions[0]);
                positions_sourcemap.push_back(old_positions_sourcemap[i + 1]);
                positions_sourcemap.push_back(old_positions_sourcemap[i + 2]);
                positions_sourcemap.push_back(old_positions_sourcemap[0]);
            } else {
                std::unreachable();
            }
        }
        positions = std::move(expanded_positions);
    }

    // Now that we have positions in Triangles order, and a mapping of new positions to old positions,
    // we can easily generate normals.
    std::vector<fvec3> normals(positions.size());
    for (size_t i = 0; i < positions.size(); i += 3) {
        fvec3 e1 = positions[i + 1] - positions[i],
              e2 = positions[i + 2] - positions[i],
              normal = normalize(cross(e1, e2));
        if (std::any_of(normal.data(), normal.data() + 3, [](float f) { return std::isnan(f); }))
            normal = { { 0.f, 1.f, 0.f } };
        for (size_t j = 0; j < 3; j++)
            normals[i + j] = normal;
    }

    // Do the same for any morph targets in here.
    fastgltf::Attribute* attr;
    std::vector<std::vector<fvec3>> morph_positions(mesh.targets.size()), morph_normals(mesh.targets.size());
    for (size_t t = 0; t < mesh.targets.size(); t++) {
        auto& acc_morph_positions = asset.accessors[mesh.findTargetAttribute(t, "POSITION")->accessorIndex];
        morph_positions[t].resize(positions.size());
        for (size_t i = 0; i < positions.size(); i++)
            memcpy(morph_positions[t][i].data(), fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, acc_morph_positions, positions_sourcemap[i]).data(), sizeof(fvec3));

        morph_normals[t].resize(positions.size());
        for (size_t i = 0; i < positions.size(); i += 3) {
            fvec3 p0 = positions[i] + morph_positions[t][i],
                  p1 = positions[i + 1] + morph_positions[t][i + 1],
                  p2 = positions[i + 2] + morph_positions[t][i + 2],
                  e1 = p1 - p0, e2 = p2 - p0,
                  normal = normalize(cross(e1, e2)), dnormal;
            if (std::any_of(normal.data(), normal.data() + 3, [](float f) { return std::isnan(f); }))
                dnormal = { 0, 0, 0 };
            else
                dnormal = normal - normals[i];
            for (size_t j = 0; j < 3; j++)
                morph_normals[t][i + j] = dnormal;
        }

        mesh.findTargetAttribute(t, "POSITION")->accessorIndex = asset.accessors.size();
        asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
        asset.accessors.back().componentType = fastgltf::ComponentType::Float;
        asset.accessors.back().count = positions.size();
        asset.accessors.back().type = fastgltf::AccessorType::Vec3;
        asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
        asset.bufferViews.back().byteLength = positions.size() * sizeof(fvec3);
        asset.bufferViews.back().byteOffset = (2 * t) * positions.size() * sizeof(fvec3);
        asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;

        if ((attr = mesh.findTargetAttribute(t, "NORMAL")) != mesh.targets[t].end())
            attr->accessorIndex = asset.accessors.size();
        else
            mesh.targets[t].emplace_back("NORMAL", asset.accessors.size());
        asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
        asset.accessors.back().componentType = fastgltf::ComponentType::Float;
        asset.accessors.back().count = positions.size();
        asset.accessors.back().type = fastgltf::AccessorType::Vec3;
        asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
        asset.bufferViews.back().byteLength = positions.size() * sizeof(fvec3);
        asset.bufferViews.back().byteOffset = (2 * t + 1) * positions.size() * sizeof(fvec3);
        asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;
    }

    mesh.findAttribute("POSITION")->accessorIndex = asset.accessors.size();
    asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
    asset.accessors.back().componentType = fastgltf::ComponentType::Float;
    asset.accessors.back().count = positions.size();
    asset.accessors.back().type = fastgltf::AccessorType::Vec3;
    asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
    asset.bufferViews.back().byteLength = positions.size() * sizeof(fvec3);
    asset.bufferViews.back().byteOffset = (2 * mesh.targets.size()) * positions.size() * sizeof(fvec3);
    asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;

    if ((attr = mesh.findAttribute("NORMAL")) != mesh.attributes.end())
        attr->accessorIndex = asset.accessors.size();
    else
        mesh.attributes.emplace_back("NORMAL", asset.accessors.size());
    asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
    asset.accessors.back().componentType = fastgltf::ComponentType::Float;
    asset.accessors.back().count = positions.size();
    asset.accessors.back().type = fastgltf::AccessorType::Vec3;
    asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
    asset.bufferViews.back().byteLength = positions.size() * sizeof(fvec3);
    asset.bufferViews.back().byteOffset = (2 * mesh.targets.size() + 1) * positions.size() * sizeof(fvec3);
    asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;

    mesh.type = fastgltf::PrimitiveType::Triangles;
    mesh.indicesAccessor = asset.accessors.size();

    std::vector<std::byte> export_data;
    if (positions.size() < std::numeric_limits<uint16_t>::max() - 1) {
        fastgltf::Accessor& new_acc_indexes = asset.accessors.emplace_back();
        new_acc_indexes.bufferViewIndex = asset.bufferViews.size();
        new_acc_indexes.componentType = fastgltf::ComponentType::UnsignedShort;
        new_acc_indexes.count = positions.size();
        new_acc_indexes.type = fastgltf::AccessorType::Scalar;

        fastgltf::BufferView& bv_indexes = asset.bufferViews.emplace_back();
        bv_indexes.bufferIndex = asset.buffers.size();
        bv_indexes.byteOffset = 2 * (mesh.targets.size() + 1) * positions.size() * sizeof(fvec3);
        bv_indexes.byteLength = positions.size() * 2;
        bv_indexes.target = fastgltf::BufferTarget::ElementArrayBuffer;

        export_data.resize(positions.size() * sizeof(fvec3) * 2 * (mesh.targets.size() + 1) + bv_indexes.byteLength);

        std::byte* indexes_ptr = export_data.data() + (positions.size() * sizeof(fvec3) * 2 * (mesh.targets.size() + 1));
        std::span<uint16_t> indexes_span = std::span(reinterpret_cast<uint16_t*>(indexes_ptr), positions.size());
        std::iota(indexes_span.begin(), indexes_span.end(), 0);
    } else {
        fastgltf::Accessor& new_acc_indexes = asset.accessors.emplace_back();
        new_acc_indexes.bufferViewIndex = asset.bufferViews.size();
        new_acc_indexes.componentType = fastgltf::ComponentType::UnsignedInt;
        new_acc_indexes.count = positions.size();
        new_acc_indexes.type = fastgltf::AccessorType::Scalar;

        fastgltf::BufferView& bv_indexes = asset.bufferViews.emplace_back();
        bv_indexes.bufferIndex = asset.buffers.size();
        bv_indexes.byteOffset = 2 * (mesh.targets.size() + 1) * positions.size() * sizeof(fvec3);
        bv_indexes.byteLength = positions.size() * 4;
        bv_indexes.target = fastgltf::BufferTarget::ElementArrayBuffer;

        export_data.resize(positions.size() * sizeof(fvec3) * 2 * (mesh.targets.size() + 1) + bv_indexes.byteLength);

        std::byte* indexes_ptr = export_data.data() + (positions.size() * sizeof(fvec3) * 2 * (mesh.targets.size() + 1));
        std::span<uint32_t> indexes_span = std::span(reinterpret_cast<uint32_t*>(indexes_ptr), positions.size());
        std::iota(indexes_span.begin(), indexes_span.end(), 0);
    }

    std::span<fvec3> export_span = std::span(reinterpret_cast<fvec3*>(export_data.data()), 2 * (mesh.targets.size() + 1) * positions.size());
    for (size_t t = 0; t < mesh.targets.size(); t++) {
        std::copy(morph_positions[t].begin(), morph_positions[t].end(), export_span.subspan(2 * t * positions.size()).begin());
        std::copy(morph_normals[t].begin(), morph_normals[t].end(), export_span.subspan((2 * t + 1) * positions.size()).begin());
    }
    std::copy(positions.begin(), positions.end(), export_span.subspan(2 * mesh.targets.size() * positions.size()).begin());
    std::copy(normals.begin(), normals.end(), export_span.subspan((2 * mesh.targets.size() + 1) * positions.size()).begin());

    fastgltf::Buffer& export_buffer = asset.buffers.emplace_back();
    export_buffer.byteLength = export_data.size();
    export_buffer.data = fastgltf::sources::Vector(std::move(export_data));
}

struct MikkUserData {
    const fastgltf::Asset& asset;
    const fastgltf::math::fvec3* positions;
    const fastgltf::math::fvec3* normals;
    const fastgltf::math::fvec2* uvs;
    const uint32_t* indexes32;
    const uint16_t* indexes16;
    const uint8_t* indexes8;

    size_t vertex_count = 0, face_count = 0;
    std::vector<fastgltf::math::fvec4> out_tangents;
};
static int mikk_getNumFaces(const SMikkTSpaceContext* context)
{
    auto* ud = static_cast<MikkUserData*>(context->m_pUserData);
    return static_cast<int>(ud->face_count);
}
static int mikk_getNumVerticesOfFace(const SMikkTSpaceContext* context, const int face)
{
    return 3;
}
static uint32_t mikk_getVertexIndex(MikkUserData* ud, const int face, const int vert)
{
    size_t i = face * 3 + vert;
    if (ud->indexes32)
        return ud->indexes32[i];
    else if (ud->indexes16)
        return ud->indexes16[i];
    else if (ud->indexes8)
        return ud->indexes8[i];
    else
        return i;
}
static void mikk_getPosition(const SMikkTSpaceContext* context, float* out, const int face, const int vert)
{
    auto* ud = static_cast<MikkUserData*>(context->m_pUserData);
    uint32_t idx = mikk_getVertexIndex(ud, face, vert);
    memcpy(out, ud->positions + idx, sizeof(fvec3));
}
static void mikk_getNormal(const SMikkTSpaceContext* context, float* out, const int face, const int vert)
{
    auto* ud = static_cast<MikkUserData*>(context->m_pUserData);
    uint32_t idx = mikk_getVertexIndex(ud, face, vert);
    memcpy(out, ud->normals + idx, sizeof(fvec3));
}
static void mikk_getTexCoord(const SMikkTSpaceContext* context, float* out, const int face, const int vert)
{
    auto* ud = static_cast<MikkUserData*>(context->m_pUserData);
    uint32_t idx = mikk_getVertexIndex(ud, face, vert);
    if (ud->uvs) {
        memcpy(out, ud->uvs + idx, sizeof(fvec2));
    } else {
        memset(out, 0, sizeof(fvec2));
    }
}
static void mikk_setTSpaceBasic(const SMikkTSpaceContext* context, const float* tangent, const float sign, const int face, const int vert)
{
    auto* ud = static_cast<MikkUserData*>(context->m_pUserData);
    uint32_t idx = mikk_getVertexIndex(ud, face, vert);
    ud->out_tangents[idx][0] = tangent[0];
    ud->out_tangents[idx][1] = tangent[1];
    ud->out_tangents[idx][2] = tangent[2];
    ud->out_tangents[idx][3] = sign;
}

void generate_mesh_tangents(fastgltf::Asset& asset, fastgltf::Primitive& mesh)
{
    MikkUserData mud { asset };
    auto& acc_base_positions = asset.accessors[mesh.findAttribute("POSITION")->accessorIndex];
    std::vector<fvec3> base_positions(acc_base_positions.count);
    fastgltf::copyFromAccessor<fvec3>(asset, acc_base_positions, base_positions.data());
    mud.positions = base_positions.data();
    mud.vertex_count = acc_base_positions.count;
    mud.out_tangents.resize(mud.vertex_count);

    auto& acc_base_normals = asset.accessors[mesh.findAttribute("NORMAL")->accessorIndex];
    std::vector<fvec3> base_normals(acc_base_normals.count);
    fastgltf::copyFromAccessor<fvec3>(asset, acc_base_normals, base_normals.data());
    mud.normals = base_normals.data();

    std::vector<fvec2> base_uv0;
    if (mesh.findAttribute("TEXCOORD_0") != mesh.attributes.end()) {
        auto& acc_uv = asset.accessors[mesh.findAttribute("TEXCOORD_0")->accessorIndex];
        base_uv0.resize(acc_uv.count);
        fastgltf::copyFromAccessor<fvec2>(asset, acc_uv, base_uv0.data());
        mud.uvs = base_uv0.data();
    }

    std::vector<std::byte> indexes;
    if (mesh.indicesAccessor) {
        auto& acc_index = asset.accessors[mesh.indicesAccessor.value()];
        mud.face_count = acc_index.count / 3;
        if (acc_index.componentType == fastgltf::ComponentType::UnsignedByte) {
            indexes.resize(acc_index.count);
            fastgltf::copyFromAccessor<uint8_t>(asset, acc_index, reinterpret_cast<uint8_t*>(indexes.data()));
            mud.indexes8 = reinterpret_cast<uint8_t*>(indexes.data());
        } else if (acc_index.componentType == fastgltf::ComponentType::UnsignedShort) {
            indexes.resize(acc_index.count * 2);
            fastgltf::copyFromAccessor<uint16_t>(asset, acc_index, reinterpret_cast<uint16_t*>(indexes.data()));
            mud.indexes16 = reinterpret_cast<uint16_t*>(indexes.data());
        } else if (acc_index.componentType == fastgltf::ComponentType::UnsignedInt) {
            indexes.resize(acc_index.count * 4);
            fastgltf::copyFromAccessor<uint32_t>(asset, acc_index, reinterpret_cast<uint32_t*>(indexes.data()));
            mud.indexes32 = reinterpret_cast<uint32_t*>(indexes.data());
        }
    } else {
        mud.face_count = mud.vertex_count / 3;
    }

    SMikkTSpaceInterface mikk_interface {};
    mikk_interface.m_getNumFaces = mikk_getNumFaces;
    mikk_interface.m_getNumVerticesOfFace = mikk_getNumVerticesOfFace;
    mikk_interface.m_getPosition = mikk_getPosition;
    mikk_interface.m_getNormal = mikk_getNormal;
    mikk_interface.m_getTexCoord = mikk_getTexCoord;
    mikk_interface.m_setTSpaceBasic = mikk_setTSpaceBasic;

    SMikkTSpaceContext mikk_context;
    mikk_context.m_pInterface = &mikk_interface;
    mikk_context.m_pUserData = &mud;
    assert(genTangSpaceDefault(&mikk_context));

    std::vector<fvec4> base_tangents = std::move(mud.out_tangents);
    std::vector<std::vector<fvec4>> morph_tangents(mesh.targets.size());
    std::vector<fvec3> dposn(mud.vertex_count), dnorm(mud.vertex_count), xposn(mud.vertex_count), xnorm(mud.vertex_count);
    mud.out_tangents.resize(mud.vertex_count);
    for (size_t t = 0; t < mesh.targets.size(); t++) {
        auto& acc_dposn = asset.accessors[mesh.findTargetAttribute(t, "POSITION")->accessorIndex];
        auto& acc_dnorm = asset.accessors[mesh.findTargetAttribute(t, "NORMAL")->accessorIndex];
        fastgltf::copyFromAccessor<fvec3>(asset, acc_dposn, dposn.data());
        fastgltf::copyFromAccessor<fvec3>(asset, acc_dnorm, dnorm.data());
        for (size_t i = 0; i < mud.vertex_count; i++) {
            xposn[i] = base_positions[i] + dposn[i];
            xnorm[i] = base_normals[i] + dnorm[i];
        }
        mud.positions = xposn.data();
        mud.normals = xnorm.data();
        assert(genTangSpaceDefault(&mikk_context));

        morph_tangents[t].resize(mud.vertex_count);
        for (size_t i = 0; i < mud.vertex_count; i++)
            morph_tangents[t][i] = mud.out_tangents[i] - base_tangents[i];
    }

    mesh.attributes.emplace_back("TANGENT", asset.accessors.size());
    asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
    asset.accessors.back().componentType = fastgltf::ComponentType::Float;
    asset.accessors.back().count = base_tangents.size();
    asset.accessors.back().type = fastgltf::AccessorType::Vec4;
    asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
    asset.bufferViews.back().byteLength = base_tangents.size() * sizeof(fvec4);
    asset.bufferViews.back().byteOffset = 0;
    asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;

    fastgltf::Attribute* attr;
    for (size_t t = 0; t < mesh.targets.size(); t++) {
        attr = mesh.findTargetAttribute(t, "TANGENT");
        if (attr != mesh.targets[t].end())
            attr->accessorIndex = asset.accessors.size();
        else
            mesh.targets[t].emplace_back("TANGENT", asset.accessors.size());

        asset.accessors.emplace_back().bufferViewIndex = asset.bufferViews.size();
        asset.accessors.back().componentType = fastgltf::ComponentType::Float;
        asset.accessors.back().count = base_tangents.size();
        asset.accessors.back().type = fastgltf::AccessorType::Vec4;
        asset.bufferViews.emplace_back().bufferIndex = asset.buffers.size();
        asset.bufferViews.back().byteLength = base_tangents.size() * sizeof(fvec4);
        asset.bufferViews.back().byteOffset = (t + 1) * base_tangents.size() * sizeof(fvec4);
        asset.bufferViews.back().target = fastgltf::BufferTarget::ArrayBuffer;
    }

    std::vector<std::byte> export_data((mesh.targets.size() + 1) * base_tangents.size() * sizeof(fvec4));
    std::span<fvec4> export_span(reinterpret_cast<fvec4*>(export_data.data()), (mesh.targets.size() + 1) * base_tangents.size());
    std::copy(base_tangents.begin(), base_tangents.end(), export_span.begin());
    for (size_t t = 0; t < mesh.targets.size(); t++)
        std::copy(morph_tangents[t].begin(), morph_tangents[t].end(), export_span.subspan((t + 1) * base_tangents.size()).begin());

    fastgltf::Buffer& export_buffer = asset.buffers.emplace_back();
    export_buffer.byteLength = export_data.size();
    export_buffer.data = fastgltf::sources::Vector(std::move(export_data));
}

void generate_indexes(fastgltf::Asset& asset, fastgltf::Primitive& mesh)
{
    auto* positionAttribute = mesh.findAttribute("POSITION");
    auto positionCount = asset.accessors[positionAttribute->accessorIndex].count;

    auto primitiveCount = [&]() -> std::size_t {
        switch (mesh.type) {
        case fastgltf::PrimitiveType::Points:
            return positionCount;
        case fastgltf::PrimitiveType::Lines:
            return positionCount / 2;
        case fastgltf::PrimitiveType::LineLoop:
        case fastgltf::PrimitiveType::LineStrip:
            return std::max<std::size_t>(0, positionCount - 1);
        case fastgltf::PrimitiveType::Triangles:
            return positionCount / 3;
        case fastgltf::PrimitiveType::TriangleStrip:
            return std::max<std::size_t>(0U, positionCount - 2);
        case fastgltf::PrimitiveType::TriangleFan:
            return std::max<std::size_t>(0U, positionCount - 2);
        default:
            std::unreachable();
        }
    }();
    auto indexCount = [&]() -> std::size_t {
        switch (mesh.type) {
        case fastgltf::PrimitiveType::Points:
            return primitiveCount;
        case fastgltf::PrimitiveType::Lines:
        case fastgltf::PrimitiveType::LineLoop:
        case fastgltf::PrimitiveType::LineStrip:
            return primitiveCount * 2;
        case fastgltf::PrimitiveType::Triangles:
        case fastgltf::PrimitiveType::TriangleStrip:
        case fastgltf::PrimitiveType::TriangleFan:
            return primitiveCount * 3;
        default:
            std::unreachable();
        }
    }();

    auto [generatedIndices, componentType] = fastgltf::writeIndices(mesh.type, indexCount, primitiveCount);

    auto bufferIdx = asset.buffers.size();
    auto& buffer = asset.buffers.emplace_back();
    buffer.byteLength = generatedIndices.size_bytes();
    fastgltf::sources::Array indicesArray {
        std::move(generatedIndices),
        fastgltf::MimeType::GltfBuffer,
    };
    buffer.data = std::move(indicesArray);

    auto bufferViewIdx = asset.bufferViews.size();
    auto& bufferView = asset.bufferViews.emplace_back();
    bufferView.byteLength = buffer.byteLength;
    bufferView.bufferIndex = bufferIdx;
    bufferView.byteOffset = 0;

    mesh.indicesAccessor = asset.accessors.size();
    auto& accessor = asset.accessors.emplace_back();
    accessor.byteOffset = 0;
    accessor.count = positionCount;
    accessor.type = fastgltf::AccessorType::Scalar;
    accessor.componentType = componentType;
    accessor.normalized = false;
    accessor.bufferViewIndex = bufferViewIdx;
}

void generate_missing_attributes(fastgltf::Asset& asset, fastgltf::Primitive& mesh)
{
    bool generated_normals = false, generated_tangents = false;
    if (mesh.findAttribute("NORMAL") == mesh.attributes.end()) {
        generate_mesh_normals(asset, mesh);
        generated_normals = true;
    }
    if (generated_normals || mesh.findAttribute("TANGENT") == mesh.attributes.end()) {
        generate_mesh_tangents(asset, mesh);
        generated_tangents = true;
    }
    if (!mesh.indicesAccessor.has_value())
        generate_indexes(asset, mesh);
}

void generate_missing_attributes(fastgltf::Asset& asset)
{
    // Mesh primitives can be handled in isolation.
    for (auto it = asset.meshes.begin(); it != asset.meshes.end(); ++it) {
        for (auto jt = it->primitives.begin(); jt != it->primitives.end(); ++jt) {
            if (jt->findAttribute("POSITION") == jt->attributes.end())
                continue;
            if (jt->type != fastgltf::PrimitiveType::Triangles && jt->type != fastgltf::PrimitiveType::TriangleStrip && jt->type != fastgltf::PrimitiveType::TriangleFan)
                continue;

            generate_missing_attributes(asset, *jt);
        }
    }
}
