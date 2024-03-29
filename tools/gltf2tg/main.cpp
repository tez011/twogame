/**
gltf2tg - convert a subset of well-formed glTF models into something loadable
 by twogame.

Extension support (planned):
 - KHR_draco_mesh_compression: Pending support by twogame.
 - KHR_materials_clearcoat: Pending support by twogame.
 - KHR_materials_emissive_strength: Pending support by twogame.
 - KHR_materials_sheen: Pending support by twogame.
 - KHR_materials_specular: Pending support by twogame.
 - KHR_materials_unlit: Pending support by twogame.

This tool does not support interleaved accessors for anything other than vertex
 attribute data. (Neither does glTF...)
This tool does not support sparse accessors.
This tool does not support embedded buffers or images (i.e. base64-encoded).
Attempting to use this tool on a glTF with these features, or that requires
 extensions that are not explicitly supported above, will result in undefined
 behavior.
**/
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <cglm/struct.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>
#include "image.hpp"

namespace fs = std::filesystem;

static const char* animations_channels_target_path(const std::string& path)
{
    if (path == "translation")
        return "translation";
    else if (path == "rotation")
        return "orientation";
    else if (path == "weights")
        return "shape-weights";
    else
        return nullptr;
}

static const char* indexes_format(int component_type)
{
    switch (component_type) {
    case 5120:
        return "int8";
    case 5121:
        return "uint8";
    case 5122:
        return "int16";
    case 5123:
        return "uint16";
    case 5125:
        return "uint32";
    default:
        assert(false);
    }
}

static std::string translate_attribute_name(const std::string& iname)
{
    if (strncmp(iname.c_str(), "TEXCOORD_", 9) == 0) {
        return "uv" + iname.substr(9);
    }

    std::string oname = iname.substr(0, iname.find('_'));
    std::transform(oname.begin(), oname.end(), oname.begin(), [](char c) { return std::tolower(c); });
    return oname;
}

class Gltf {
    struct Accessor {
        std::ifstream* buffer;
        size_t offset, count;
        int component_type, width;
        uint16_t i_offset, i_stride;

        size_t byte_width() const
        {
            switch (component_type) {
            case 5120:
            case 5121:
                return width;
            case 5122:
            case 5123:
                return width * 2;
            case 5125:
            case 5126:
                return width * 4;
            default:
                assert(false);
            }
        }

        std::string vkformat() const
        {
            if (width > 4)
                return "INVALID";

            size_t atom_size;
            const char *suffix, *colors = "RGBA";
            switch (component_type) {
            case 5120:
                atom_size = 1;
                suffix = "SINT";
                break;
            case 5121:
                atom_size = 1;
                suffix = "UINT";
                break;
            case 5122:
                atom_size = 2;
                suffix = "SINT";
                break;
            case 5123:
                atom_size = 2;
                suffix = "UINT";
                break;
            case 5125:
                atom_size = 4;
                suffix = "UINT";
                break;
            case 5126:
                atom_size = 4;
                suffix = "SFLOAT";
                break;
            default:
                assert(false);
            }

            std::ostringstream oss;
            for (int i = 0; i < width; i++)
                oss << colors[i] << atom_size * 8;
            oss << "_" << suffix;
            return oss.str();
        }

        bool interleaved() const { return i_stride != 0; }
    };
    struct Animation {
        struct Channel {
            size_t output, node;
            std::string path;
        };
        std::map<size_t, std::deque<Channel>> channels;
        std::map<size_t, std::string> methods;
    };
    ImageGenerator makeimage;
    fs::path sourcedir;
    nlohmann::json idoc;
    std::vector<char> scratch;
    std::vector<std::ifstream> buffers;
    std::vector<Accessor> accessors;
    std::vector<Animation> animations;
    std::vector<bool> image_usage, material_usage;
    std::vector<size_t> accessor_usage;
    std::string stem;
    int max_index_type = 0;

public:
    Gltf(nlohmann::json&& doc, const fs::path& indir)
        : sourcedir(indir)
        , idoc(std::move(doc))
    {
    }

    inline ImageGenerator& set_makeimage() { return makeimage; }

    int load()
    {
        if (!idoc.contains("accessors") || !idoc.contains("bufferViews") || !idoc.contains("buffers") || !idoc.contains("meshes"))
            return __LINE__;

        scratch.resize(1 << 22);
        accessors.resize(idoc["accessors"].size());
        accessor_usage.reserve(accessors.size());
        buffers.resize(idoc["buffers"].size());
        if (idoc.contains("animations"))
            animations.resize(idoc["animations"].size());
        if (idoc.contains("images"))
            image_usage.resize(idoc["images"].size());
        if (idoc.contains("materials"))
            material_usage.resize(idoc["materials"].size());

        for (size_t i = 0; i < idoc["buffers"].size(); i++)
            buffers[i] = std::ifstream(sourcedir / idoc["buffers"][i]["uri"].template get<nlohmann::json::string_t>());
        for (size_t i = 0; i < idoc["accessors"].size(); i++) {
            const auto& a = idoc["accessors"][i];
            const auto& bv = idoc["bufferViews"][a["bufferView"].template get<size_t>()];
            const auto& bc = idoc["buffers"][bv["buffer"].template get<size_t>()];
            std::string a_type = a["type"];
            accessors[i].buffer = &buffers[bv["buffer"].template get<size_t>()];
            accessors[i].offset = bc.value("byteOffset", 0) + bv.value("byteOffset", 0) + a.value("byteOffset", 0);
            accessors[i].count = a["count"];
            accessors[i].component_type = a["componentType"];
            if (strcmp(a_type.c_str(), "SCALAR") == 0)
                accessors[i].width = 1;
            else if (strncmp(a_type.c_str(), "VEC", 3) == 0)
                accessors[i].width = a_type[3] - '0';
            else if (strncmp(a_type.c_str(), "MAT", 3) == 0)
                accessors[i].width = (a_type[3] - '0') * (a_type[3] - '0');
            else
                return __LINE__;

            uint8_t buffer_view_stride = bv.value("byteStride", 0);
            if (buffer_view_stride && accessors[i].byte_width() != buffer_view_stride) {
                accessors[i].offset = bv.value("byteOffset", 0);
                accessors[i].i_offset = a.value("byteOffset", 0);
                accessors[i].i_stride = buffer_view_stride;
            }
        }

        for (size_t i = 0; i < animations.size(); i++) {
            for (auto it = idoc["animations"][i]["channels"].begin(); it != idoc["animations"][i]["channels"].end(); ++it) {
                const auto& sampler = idoc["animations"][i]["samplers"][it->at("sampler").template get<size_t>()];
                size_t input_accessor = sampler["input"];
                auto jt = animations[i].methods.find(input_accessor);
                if (sampler.contains("interpolation")) {
                    if (jt == animations[i].methods.end() || jt->second.empty())
                        animations[i].methods[input_accessor] = sampler["interpolation"];
                    else if (jt->second != sampler["interpolation"])
                        return __LINE__;
                } else if (jt == animations[i].methods.end())
                    animations[i].methods[input_accessor] = "";

                Animation::Channel& c = animations[i].channels[input_accessor].emplace_back();
                c.output = sampler["output"];
                c.node = it->at("target").value("node", std::numeric_limits<size_t>::max());
                c.path = it->at("target")["path"];
            }
        }

        return 0;
    }

    template <typename in_t, typename out_t>
    void write_index_buffer(std::ostream& outbin, const Accessor& acc)
    {
        if constexpr (std::is_same_v<in_t, out_t>) {
            std::copy_n(std::istreambuf_iterator<char>(*acc.buffer), acc.count * acc.byte_width(), std::ostreambuf_iterator<char>(outbin));
        } else {
            ssize_t remaining = acc.count, chunk_size = scratch.size() / 8;
            char *inbuf = scratch.data(), *outbuf = scratch.data() + scratch.size() / 2;
            in_t* indata = reinterpret_cast<in_t*>(inbuf);
            out_t* outdata = reinterpret_cast<out_t*>(outbuf);
            while (remaining > 0) {
                size_t tcs = std::min(remaining, chunk_size);
                acc.buffer->read(inbuf, tcs * sizeof(in_t));
                std::copy(indata, indata + tcs, outdata);
                outbin.write(outbuf, tcs * sizeof(out_t));
                remaining -= tcs;
            }
        }
    }

    template <typename out_t>
    void write_index_buffer(std::ostream& outbin, const Accessor& acc)
    {
        switch (acc.component_type) {
        case 5120:
        case 5121:
            return write_index_buffer<uint8_t, out_t>(outbin, acc);
        case 5122:
        case 5123:
            return write_index_buffer<uint16_t, out_t>(outbin, acc);
        case 5125:
            return write_index_buffer<uint32_t, out_t>(outbin, acc);
        default:
            assert(false);
        }
    }

    void write_index_buffer(std::ostream& outbin, const Accessor& acc)
    {
        switch (max_index_type) {
        case 5120:
        case 5121:
            return write_index_buffer<uint8_t>(outbin, acc);
        case 5122:
        case 5123:
            return write_index_buffer<uint16_t>(outbin, acc);
        case 5125:
            return write_index_buffer<uint32_t>(outbin, acc);
        default:
            assert(false);
        }
    }

    void write_displacements(std::ostream& outbin, const Accessor& acc)
    {
        // guarantee: stbuf is of size 1 << 21
        float* ffb = reinterpret_cast<float*>(scratch.data());
        size_t remaining = acc.count;
        while (remaining > 0) {
            acc.buffer->read(scratch.data(), std::min(remaining * 12, scratch.size() * 3 / 4));
            size_t items = acc.buffer->gcount() / 12;
            for (ssize_t i = items - 1; i >= 0; i--) {
                ffb[i * 4 + 3] = 0;
                ffb[i * 4 + 2] = ffb[i * 3 + 2];
                ffb[i * 4 + 1] = ffb[i * 3 + 1];
                ffb[i * 4 + 0] = ffb[i * 3 + 0];
            }
            outbin.write(scratch.data(), items * 16);
            remaining -= items;
        }
    }

    int convert(const fs::path& outparam)
    {
        fs::path outdir = fs::absolute(outparam.parent_path());
        stem = outparam.stem();

        pugi::xml_document outdoc;
        std::string outbin_name = stem + ".bin";
        std::ofstream outbin(outdir / outbin_name, std::ios_base::binary);
        std::set<pugi::xml_node> rewrite_range;
        std::set<size_t> index_accessors, displacement_accessors;

        pugi::xml_node asset_root = outdoc.append_child("assets");
        asset_root.append_attribute("source").set_value(outbin_name.c_str());
        for (size_t i = 0; i < idoc["meshes"].size(); i++) {
            pugi::xml_node mesh = asset_root.append_child("mesh");
            mesh.append_attribute("name").set_value(idoc["meshes"][i].value("name", fmt::format("{}.m{}", stem, i)).c_str());

            for (auto pt = idoc["meshes"][i]["primitives"].begin(); pt != idoc["meshes"][i]["primitives"].end(); ++pt) {
                pugi::xml_node mp = mesh.append_child("primitives");

                // Write interleaved buffer views first and directly to avoid collision.
                size_t interleaved_accessors = 0, attr_count = 0;
                for (auto it = pt->at("attributes").begin(); it != pt->at("attributes").end(); ++it) {
                    size_t aci = it.value();
                    if (accessors[aci].interleaved() == false)
                        continue;
                    if (accessors[aci].i_offset)
                        continue;

                    const auto& common_bufferview = idoc["bufferViews"][idoc["accessors"][aci]["bufferView"].template get<size_t>()];
                    std::deque<size_t> common_accessors;
                    common_accessors.push_front(aci);
                    for (size_t j = 0; j < accessors.size(); j++) {
                        if (j == aci)
                            continue;
                        if (accessors[j].buffer == accessors[aci].buffer && accessors[j].offset == accessors[aci].offset)
                            common_accessors.push_back(j);
                    }
                    std::sort(common_accessors.begin() + 1, common_accessors.end(),
                        [&](size_t aL, size_t aR) { return accessors[aL].i_offset < accessors[aR].i_offset; });

                    pugi::xml_node attributes = mp.append_child("attributes");
                    attributes.append_attribute("range").set_value(fmt::format("{} {}", static_cast<size_t>(outbin.tellp()), common_bufferview["byteLength"].template get<size_t>()).c_str());
                    attributes.append_attribute("interleaved").set_value("true");
                    if (attr_count && attr_count != accessors[aci].count)
                        return __LINE__;
                    else
                        attr_count = accessors[aci].count;

                    for (auto jt = common_accessors.begin(); jt != common_accessors.end(); ++jt) {
                        std::string attr_name;
                        for (auto kt = pt->at("attributes").begin(); attr_name.empty() && kt != pt->at("attributes").end(); ++kt) {
                            if (kt.value() == *jt)
                                attr_name = translate_attribute_name(kt.key());
                        }
                        if (attr_name.empty())
                            return __LINE__;

                        pugi::xml_node attribute = attributes.append_child("attribute");
                        attribute.append_attribute("name").set_value(attr_name.c_str());
                        attribute.append_attribute("format").set_value(accessors[*jt].vkformat().c_str());
                        interleaved_accessors++;
                    }

                    accessors[aci].buffer->seekg(accessors[aci].offset);
                    std::copy_n(std::istreambuf_iterator<char>(*accessors[aci].buffer), common_bufferview["byteLength"].template get<size_t>(), std::ostreambuf_iterator(outbin));
                }

                if (interleaved_accessors < pt->at("attributes").size()) {
                    pugi::xml_node attributes = mp.append_child("attributes");
                    size_t attr_range_begin = accessor_usage.size();

                    for (auto it = pt->at("attributes").begin(); it != pt->at("attributes").end(); ++it) {
                        size_t aci = it.value();
                        if (accessors[aci].interleaved())
                            continue;

                        pugi::xml_node attribute = attributes.append_child("attribute");
                        attribute.append_attribute("name").set_value(translate_attribute_name(it.key()).c_str());
                        attribute.append_attribute("format").set_value(accessors[aci].vkformat().c_str());
                        if (attr_count && attr_count != accessors[aci].count)
                            return __LINE__;
                        else
                            attr_count = accessors[aci].count;
                        accessor_usage.push_back(aci);
                    }
                    attributes.append_attribute("range").set_value(fmt::format("{} {}", attr_range_begin, accessor_usage.size()).c_str());
                    attributes.append_attribute("interleaved").set_value("false");
                    rewrite_range.insert(attributes);
                }
                mp.append_attribute("count").set_value(attr_count);

                if (pt->contains("indices")) {
                    pugi::xml_node indexes = mp.append_child("indexes");
                    size_t index_accessor = pt->at("indices");
                    const Accessor& iacc = accessors[index_accessor];
                    if (iacc.component_type == 5126)
                        return __LINE__;
                    else
                        max_index_type = std::max(max_index_type, iacc.component_type);

                    indexes.append_attribute("count").set_value(iacc.count);
                    indexes.append_attribute("range").set_value(fmt::format("{} {}", accessor_usage.size(), accessor_usage.size() + 1).c_str());
                    accessor_usage.push_back(index_accessor);
                    index_accessors.insert(index_accessor);
                    rewrite_range.insert(indexes);
                }

                if (pt->contains("material"))
                    material_usage[pt->at("material").template get<size_t>()] = true;
                if (pt->contains("targets")) {
                    std::map<std::string, std::deque<size_t>> morph_targets;
                    for (auto it = pt->at("targets").begin(); it != pt->at("targets").end(); ++it) {
                        for (auto jt = it->begin(); jt != it->end(); ++jt) {
                            morph_targets[translate_attribute_name(jt.key())].push_back(jt.value());
                            displacement_accessors.insert(jt.value().template get<size_t>());
                        }
                    }

                    for (auto it = morph_targets.begin(); it != morph_targets.end(); ++it) {
                        pugi::xml_node displacements = mp.append_child("displacements");
                        size_t attr_range_begin = accessor_usage.size();
                        accessor_usage.insert(accessor_usage.end(), it->second.begin(), it->second.end());

                        displacements.append_attribute("name").set_value(it->first.c_str());
                        displacements.append_attribute("range").set_value(fmt::format("{} {}", attr_range_begin, accessor_usage.size()).c_str());
                        rewrite_range.insert(displacements);
                    }
                }
            }
            if (idoc["meshes"][i].contains("weights")) {
                pugi::xml_node initial_weights = mesh.append_child("shape-weights");
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(4);
                for (auto it = idoc["meshes"][i]["weights"].begin(); it != idoc["meshes"][i]["weights"].end(); ++it)
                    oss << ' ' << it->template get<float>();
                initial_weights.append_child(pugi::node_pcdata).set_value(oss.str().c_str() + 1);
            }
        }

        // Extract skeletons
        std::vector<size_t> nodes_to_bones(idoc["nodes"].size());
        for (size_t i = 0; idoc.contains("skins") && i < idoc["skins"].size(); i++) {
            pugi::xml_node skin = asset_root.append_child("skeleton");
            skin.append_attribute("name").set_value(idoc["skins"][i].value("name", fmt::format("{}.sk{}", stem, i)).c_str());
            skin.append_attribute("range").set_value(fmt::format("{} {}", accessor_usage.size(), accessor_usage.size() + 1).c_str());
            accessor_usage.push_back(idoc["skins"][i]["inverseBindMatrices"].template get<size_t>());
            rewrite_range.insert(skin);

            std::map<size_t, size_t> joint_parents;
            std::queue<size_t> nodeQ;
            nodeQ.push(idoc["skins"][i].value("skeleton", 0));
            while (nodeQ.empty() == false) {
                size_t current = nodeQ.front();
                nodeQ.pop();

                if (idoc["nodes"][current].contains("children")) {
                    for (auto it = idoc["nodes"][current]["children"].begin(); it != idoc["nodes"][current]["children"].end(); ++it) {
                        nodeQ.push(*it);
                        joint_parents[*it] = current;
                    }
                }
            }

            std::vector<size_t> joint_list = idoc["skins"][i]["joints"];
            for (size_t j = 0; j < joint_list.size(); j++) {
                pugi::xml_node joint = skin.append_child("joint");
                auto pit = joint_parents.find(joint_list[j]);
                if (pit != joint_parents.end())
                    joint.append_attribute("parent").set_value(std::find(joint_list.begin(), joint_list.end(), pit->second) - joint_list.begin() + 1);
                nodes_to_bones[joint_list[j]] = j + 1;

                vec4 zero4 = GLM_VEC4_ZERO_INIT, t;
                vec3 one3 = GLM_VEC3_ONE_INIT, s;
                versors q;
                if (idoc["nodes"][joint_list[j]].contains("matrix")) {
                    std::array<float, 16> mdata;
                    for (int k = 0; k < mdata.size(); k++)
                        mdata[k] = idoc["nodes"][joint_list[j]]["matrix"][k];

                    mat4 m, r;
                    glm_mat4_make(mdata.data(), m);
                    glm_decompose(m, t, r, s);
                    glm_mat4_quat(r, q.raw);
                    joint.append_attribute("translation").set_value(fmt::format("{} {} {}", t[0], t[1], t[2]).c_str());
                    joint.append_attribute("orientation").set_value(fmt::format("{} {} {} {}", q.x, q.y, q.z, q.w).c_str());
                    if (!glm_vec3_eqv_eps(s, one3))
                        joint.append_attribute("scale").set_value(fmt::format("{} {} {}", s[0], s[1], s[2]).c_str());
                } else {
                    if (idoc["nodes"][joint_list[j]].contains("translation")) {
                        for (int k = 0; k < 3; k++)
                            t[k] = idoc["nodes"][joint_list[j]]["translation"][k].template get<float>();
                        joint.append_attribute("translation").set_value(fmt::format("{} {} {}", t[0], t[1], t[2]).c_str());
                    } else
                        joint.append_attribute("translation").set_value("0 0 0");
                    if (idoc["nodes"][joint_list[j]].contains("rotation")) {
                        versors q = glms_quat_init(idoc["nodes"][joint_list[j]]["rotation"][0].template get<float>(),
                            idoc["nodes"][joint_list[j]]["rotation"][1].template get<float>(),
                            idoc["nodes"][joint_list[j]]["rotation"][2].template get<float>(),
                            idoc["nodes"][joint_list[j]]["rotation"][3].template get<float>());
                        glms_quat_normalize(q);
                        joint.append_attribute("orientation").set_value(fmt::format("{} {} {} {}", q.x, q.y, q.z, q.w).c_str());
                    } else
                        joint.append_attribute("orientation").set_value("0 0 0 1");
                    if (idoc["nodes"][joint_list[j]].contains("scale")) {
                        for (int k = 0; k < 3; k++)
                            s[k] = idoc["nodes"][joint_list[j]]["scale"][k].template get<float>();
                        if (!glm_vec3_eqv_eps(s, one3))
                            joint.append_attribute("scale").set_value(fmt::format("{} {} {}", s[0], s[1], s[2]).c_str());
                    }
                }
            }
        }

        // Extract animations
        for (size_t i = 0; i < animations.size(); i++) {
            std::string anim_stem = idoc["animations"][i].value("name", fmt::format("{}.an{}", stem, i));
            size_t anim_counter = 1;
            for (auto it = animations[i].channels.begin(); it != animations[i].channels.end(); ++it, ++anim_counter) {
                pugi::xml_node animation = asset_root.append_child("animation");
                auto method_it = animations[i].methods.find(it->first);
                animation.append_attribute("name").set_value(animations[i].channels.size() == 1 ? anim_stem.c_str() : fmt::format("{}.{}", anim_stem, anim_counter).c_str());
                animation.append_attribute("range");
                animation.append_attribute("keyframes").set_value(accessors[it->first].count);
                if (method_it != animations[i].methods.end()) {
                    std::string method = method_it->second;
                    std::transform(method.begin(), method.end(), method.begin(), [](char c) { return std::tolower(c); });
                    if (!method.empty())
                        animation.append_attribute("method").set_value(method.c_str());
                }

                size_t range_start = accessor_usage.size();
                accessor_usage.push_back(it->first);
                for (auto jt = it->second.begin(); jt != it->second.end(); ++jt) {
                    const char* target = animations_channels_target_path(jt->path);
                    if (target == nullptr)
                        continue;

                    pugi::xml_node anim_output = animation.append_child("output");
                    anim_output.append_attribute("target").set_value(target);
                    if (jt->path == "weights")
                        anim_output.append_attribute("width").set_value(accessors[jt->output].count / accessors[it->first].count);
                    else if (jt->node != std::numeric_limits<size_t>::max())
                        anim_output.append_attribute("bone").set_value(nodes_to_bones[jt->node]);
                    accessor_usage.push_back(jt->output);
                }
                animation.attribute("range").set_value(fmt::format("{} {}", range_start, accessor_usage.size()).c_str());
                rewrite_range.insert(animation);
            }
        }

        // Extract materials
        for (int i = 0; i < material_usage.size(); i++) {
            if (material_usage[i] == false)
                continue;

            pugi::xml_node material = asset_root.append_child("material");
            material.append_attribute("name").set_value(idoc["materials"][i].value("name", fmt::format("{}.mtl{}", stem, i)).c_str());
            if (idoc["materials"][i].contains("alphaMode")) {
                material.append_attribute("alpha-mask").set_value(idoc["materials"][i]["alphaMode"] == "MASK");
            }
            if (idoc["materials"][i].value("doubleSided", false))
                material.append_attribute("double-sided").set_value("true");

            if (idoc["materials"][i].contains("pbrMetallicRoughness")) {
                const auto& pbrMR = idoc["materials"][i]["pbrMetallicRoughness"];
                if (pbrMR.contains("baseColorFactor")) {
                    pugi::xml_node binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("base_color_factor");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{} {} {} {}", pbrMR["baseColorFactor"][0].template get<float>(), pbrMR["baseColorFactor"][1].template get<float>(), pbrMR["baseColorFactor"][2].template get<float>(), pbrMR["baseColorFactor"][3].template get<float>()).c_str());
                }
                if (pbrMR.contains("baseColorTexture")) {
                    pugi::xml_node binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("base_color_texture");
                    binding.append_attribute("type").set_value("image-sampler");

                    size_t image_index = idoc["textures"][pbrMR["baseColorTexture"]["index"].template get<size_t>()]["source"];
                    image_usage[image_index] = true;
                    binding.append_child(pugi::node_pcdata).set_value(idoc["images"][image_index].value("name", fmt::format("{}.i{}", stem, image_index)).c_str());

                    if (pbrMR["baseColorTexture"].contains("texCoord")) {
                        binding = material.append_child("binding");
                        binding.append_attribute("name").set_value("base_color_texture_coordinates");
                        binding.append_attribute("type").set_value("uint");
                        binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", pbrMR["baseColorTexture"]["texCoord"].template get<uint32_t>()).c_str());
                    }
                }
                if (pbrMR.contains("metallicFactor")) {
                    pugi::xml_node binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("metallic_factor");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", pbrMR["metallicFactor"].template get<float>()).c_str());
                }
                if (pbrMR.contains("roughnessFactor")) {
                    pugi::xml_node binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("roughness_factor");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", pbrMR["roughnessFactor"].template get<float>()).c_str());
                }
                if (pbrMR.contains("metallicRoughnessTexture")) {
                    pugi::xml_node binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("metallic_roughness_texture");
                    binding.append_attribute("type").set_value("image-sampler");

                    size_t image_index = idoc["textures"][pbrMR["metallicRoughnessTexture"]["index"].template get<size_t>()]["source"];
                    image_usage[image_index] = true;
                    binding.append_child(pugi::node_pcdata).set_value(idoc["images"][image_index].value("name", fmt::format("{}.i{}", stem, image_index)).c_str());

                    if (pbrMR["metallicRoughnessTexture"].contains("texCoord")) {
                        binding = material.append_child("binding");
                        binding.append_attribute("name").set_value("metallic_roughness_texture_coordinates");
                        binding.append_attribute("type").set_value("uint");
                        binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", pbrMR["metallicRoughnessTexture"]["texCoord"].template get<uint32_t>()).c_str());
                    }
                }
            }
            if (idoc["materials"][i].contains("normalTexture")) {
                pugi::xml_node binding = material.append_child("binding");
                binding.append_attribute("name").set_value("normal_texture");
                binding.append_attribute("type").set_value("image-sampler");

                size_t image_index = idoc["textures"][idoc["materials"][i]["normalTexture"]["index"].template get<size_t>()]["source"];
                image_usage[image_index] = true;
                binding.append_child(pugi::node_pcdata).set_value(idoc["images"][image_index].value("name", fmt::format("{}.i{}", stem, image_index)).c_str());

                if (idoc["materials"][i]["normalTexture"].contains("texCoord")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("normal_texture_coordinates");
                    binding.append_attribute("type").set_value("uint");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["normalTexture"]["texCoord"].template get<uint32_t>()).c_str());
                }
                if (idoc["materials"][i]["normalTexture"].contains("scale")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("normal_texture_scale_factor");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["normalTexture"]["scale"].template get<float>()).c_str());
                }
            }
            if (idoc["materials"][i].contains("occlusionTexture")) {
                pugi::xml_node binding = material.append_child("binding");
                binding.append_attribute("name").set_value("occlusion_texture");
                binding.append_attribute("type").set_value("image-sampler");

                size_t image_index = idoc["textures"][idoc["materials"][i]["occlusionTexture"]["index"].template get<size_t>()]["source"];
                image_usage[image_index] = true;
                binding.append_child(pugi::node_pcdata).set_value(idoc["images"][image_index].value("name", fmt::format("{}.i{}", stem, image_index)).c_str());

                if (idoc["materials"][i]["occlusionTexture"].contains("texCoord")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("occlusion_texture_coordinates");
                    binding.append_attribute("type").set_value("uint");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["occlusionTexture"]["texCoord"].template get<uint32_t>()).c_str());
                }
                if (idoc["materials"][i]["occlusionTexture"].contains("strength")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("occlusion_texture_strength");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["occlusionTexture"]["strength"].template get<float>()).c_str());
                }
            }
            if (idoc["materials"][i].contains("emissiveTexture")) {
                pugi::xml_node binding = material.append_child("binding");
                binding.append_attribute("name").set_value("emissive_texture");
                binding.append_attribute("type").set_value("image-sampler");

                size_t image_index = idoc["textures"][idoc["materials"][i]["emissiveTexture"]["index"].template get<size_t>()]["source"];
                image_usage[image_index] = true;
                binding.append_child(pugi::node_pcdata).set_value(idoc["images"][image_index].value("name", fmt::format("{}.i{}", stem, image_index)).c_str());

                if (idoc["materials"][i]["emissiveTexture"].contains("texCoord")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("emissive_texture_coordinates");
                    binding.append_attribute("type").set_value("uint");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["emissiveTexture"]["texCoord"].template get<size_t>()).c_str());
                }
                if (idoc["materials"][i].contains("emissiveFactor")) {
                    binding = material.append_child("binding");
                    binding.append_attribute("name").set_value("emissive_factor");
                    binding.append_attribute("type").set_value("float");
                    binding.append_child(pugi::node_pcdata).set_value(fmt::format("{} {} {}", idoc["materials"][i]["emissiveFactor"][0].template get<float>(), idoc["materials"][i]["emissiveFactor"][1].template get<float>(), idoc["materials"][i]["emissiveFactor"][2].template get<float>()).c_str());
                }
            }
            if (idoc["materials"][i].contains("alphaCutoff")) {
                pugi::xml_node binding = material.append_child("binding");
                binding.append_attribute("name").set_value("alpha_cutoff");
                binding.append_attribute("type").set_value("float");
                binding.append_child(pugi::node_pcdata).set_value(fmt::format("{}", idoc["materials"][i]["alphaCutoff"].template get<float>()).c_str());
            }
        }

        for (int i = 0; i < image_usage.size(); i++) {
            if (!image_usage[i])
                continue;

            std::string out_name = fmt::format("{}.{}.ktx2", stem, idoc["images"][i].value("name", fmt::format("i{}", i)));
            if (idoc["images"][i].contains("bufferView")) {
                const auto& bv = idoc["bufferViews"][idoc["images"][i]["bufferView"].template get<size_t>()];
                size_t buffer_index = bv["buffer"].template get<size_t>();
                std::vector<char> imagedata(bv["byteLength"].template get<size_t>());
                buffers[buffer_index].seekg(idoc["buffers"][buffer_index].value("byteOffset", 0) + bv.value("byteOffset", 0));
                buffers[buffer_index].read(imagedata.data(), imagedata.size());
                makeimage.generate(outdir / out_name, reinterpret_cast<unsigned char*>(imagedata.data()), imagedata.size(), idoc["images"][i]["mimeType"].template get<std::string>());
            } else if (idoc["images"][i].contains("uri")) {
                makeimage.generate(outdir / out_name, sourcedir / idoc["images"][i]["uri"]);
            } else {
                continue;
            }

            pugi::xml_node image = asset_root.append_child("image");
            image.append_attribute("name").set_value(idoc["images"][i].value("name", out_name.substr(0, out_name.length() - 5)).c_str());
            image.append_attribute("usage").set_value("sampled");
            image.append_attribute("source").set_value(out_name.c_str());
        }

        std::vector<size_t> accessor_final_offsets;
        accessor_final_offsets.reserve(accessor_usage.size());
        for (auto it = accessor_usage.begin(); it != accessor_usage.end(); ++it) {
            const Accessor& acc = accessors[*it];
            if (acc.interleaved())
                return __LINE__;

            accessor_final_offsets.push_back(outbin.tellp());
            acc.buffer->seekg(acc.offset);
            if (index_accessors.contains(*it)) {
                write_index_buffer(outbin, acc);
            } else if (displacement_accessors.contains(*it)) {
                write_displacements(outbin, acc);
            } else {
                std::copy_n(std::istreambuf_iterator<char>(*acc.buffer), acc.count * acc.byte_width(), std::ostreambuf_iterator<char>(outbin));
            }
        }
        accessor_final_offsets.push_back(outbin.tellp()); // end

        for (auto it = rewrite_range.begin(); it != rewrite_range.end(); ++it) {
            size_t rr[2];
            if (it->attribute("range")) {
                if (sscanf(it->attribute("range").value(), "%zu %zu", rr + 0, rr + 1) != 2)
                    return __LINE__;

                it->attribute("range").set_value(fmt::format("{} {}", accessor_final_offsets[rr[0]], accessor_final_offsets[rr[1]] - accessor_final_offsets[rr[0]]).c_str());
            }
            if (it->attribute("offset")) {
                if (sscanf(it->attribute("offset").value(), "%zu", rr) != 1)
                    return __LINE__;

                it->attribute("offset").set_value(accessor_final_offsets[*rr]);
                if (index_accessors.contains(*rr))
                    it->attribute("format").set_value(indexes_format(max_index_type));
            }
        }

        std::ofstream outdocstream(outparam);
        outdoc.save(outdocstream);
        return 0;
    }
};

nlohmann::json parse_gltf(const fs::path& infile)
{
    using json = nlohmann::json;

    std::ifstream instream(infile, std::ios_base::binary);
    char header[16];
    instream.read(header, 12);
    if (memcmp(header + 0, "glTF", 4) == 0 && header[4] == 2 && header[5] == 0 && header[6] == 0 && header[7] == 0) {
        uint32_t chunk_length;
        instream.read(header, 8);
        if (!instream.good())
            return json::value_t::discarded;
        chunk_length = (header[0] << 0) | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
        if (memcmp(header + 4, "JSON", 4))
            return json::value_t::discarded;

        std::string chunk_data(chunk_length, 0);
        instream.read(chunk_data.data(), chunk_length);
        json gltf = json::parse(chunk_data, nullptr, false, false);
        if (!gltf.is_object())
            return json::value_t::discarded;

        size_t glb_buffer = SIZE_MAX;
        for (size_t i = 0; i < gltf["buffers"].size(); i++) {
            if (!gltf["buffers"][i].contains("uri")) {
                if (glb_buffer == SIZE_MAX)
                    glb_buffer = i;
                else
                    return json::value_t::discarded;
            }
        }
        if (glb_buffer != SIZE_MAX) {
            instream.read(header, 8);
            if (!instream.good())
                return json::value_t::discarded;

            uint32_t buffer_length = gltf["buffers"][glb_buffer]["byteLength"];
            chunk_length = (header[0] << 0) | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
            if (chunk_length != (buffer_length + 3) & ~3) // account for padding
                return json::value_t::discarded;
            gltf["buffers"][glb_buffer]["uri"] = fs::absolute(infile);
            gltf["buffers"][glb_buffer]["byteOffset"] = static_cast<size_t>(instream.tellg());
        }
        return gltf;
    } else {
        instream.seekg(0);
        return json::parse(instream, nullptr, false, false);
    }
}

int usage(const char* argv0, const std::string& extra = "")
{
    std::cerr << "usage: " << argv0 << " [--uastc] -o OUTFILE INFILE"
              << "\n\tOUTFILE should have an .xml extension"
              << "\n\tINFILE must be a valid glTF file in either JSON or GLB format."
              << "\n\tUsing this tool with invalid inputs may result in undefined behavior.";
    if (extra.empty() == false)
        std::cerr << "\n\n\t" << extra;
    std::cerr << std::endl;
    return 1;
}

int main(int argc, char** argv)
{
    fs::path infile, outparam;
    bool enable_uastc = false;
    for (int i = 0; ++i < argc;) {
        if (strcmp(argv[i], "-o") == 0) {
            outparam = argv[++i];
        } else if (argv[i], "--uastc" == 0) {
            enable_uastc = true;
        } else {
            infile = argv[i];
        }
    }

    assert(volkInitialize() == VK_SUCCESS);
    if (infile.empty() || outparam.empty())
        return usage(*argv);

    fs::path outdir = fs::absolute(outparam.parent_path());
    if (fs::is_directory(outdir) == false)
        return usage(*argv, "OUTFILE: file not found");
    if (fs::is_regular_file(infile) == false)
        return usage(*argv, "INFILE: file not found");

    nlohmann::json mdata = parse_gltf(infile);
    if (mdata.is_discarded()) {
        std::cerr << infile << ": invalid glTF" << std::endl;
        return 1;
    }

    int status;
    Gltf imodel(std::move(mdata), infile.parent_path());
    imodel.set_makeimage().set_enable_uastc(enable_uastc);
    if ((status = imodel.load()) != 0)
        return status;
    else if ((status = imodel.convert(outparam)) != 0)
        return status;
    else
        return 0;
}
