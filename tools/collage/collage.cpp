#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <fastgltf/core.hpp>
#include <pugixml.hpp>
#include "scene.h"

namespace fs = std::filesystem;
namespace fbs = twogame::fbs;

extern bool image_enable_uastc;
extern size_t max_uv_channels, max_color_channels, max_joint_channels;

extern void generate_missing_attributes(fastgltf::Asset& asset);
extern void load_gltf(OutputScene& scene, const fastgltf::Asset& asset);

static int usage(const char* argv0, std::string_view extra = "")
{
    std::cerr << "usage: " << argv0 << " [-s] -o OUTFILE INFILE" << std::endl
              << "\t-s: enable UASTC compression of image mip levels" << std::endl
              << "\tINFILE must be a valid glTF file." << std::endl;
    if (extra.empty() == false)
        std::cerr << std::endl
                  << '\t' << extra << std::endl;
    return 1;
}

OutputScene::OutputScene(const std::string& asset_path)
{
    size_t ext_offset = asset_path.find_last_of('.');
    m_asset_path = asset_path.substr(0, ext_offset);
}

size_t OutputScene::push_ibuffer(std::span<const std::byte> data)
{
    std::vector<char> out_name(m_asset_path.length() + 16);
    m_scene.buffers.push_back(data.size_bytes());

    size_t index = m_scene.buffers.size();
    snprintf(out_name.data(), out_name.size(), "%s.%zu.bin", m_asset_path.c_str(), index);

    std::ofstream slice_stream(out_name.data(), std::ios::binary | std::ios::out);
    slice_stream.write(reinterpret_cast<const char*>(data.data()), data.size_bytes());
    slice_stream.close();
    return index;
}

void OutputScene::finish()
{
    fs::path ofs_path = m_asset_path;
    ofs_path.replace_extension(".tgs");

    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(fbs::Scene::Pack(fbb, &m_scene));

    std::ofstream ofs(ofs_path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    ofs.close();
}

void load_scene_extensions(OutputScene& oscene, const pugi::xml_node& graph)
{
    for (auto it = graph.children("node").begin(); it != graph.children("node").end(); ++it) {
        std::string node_name = it->attribute("name").as_string();
        size_t node_index;
        fbs::SceneNodeT* node;
        auto node_it = node_name.size() > 0 ? oscene.node_names.find(node_name) : oscene.node_names.end();
        if (node_it == oscene.node_names.end()) {
            node_index = oscene->nodes.size();
            node = oscene->nodes.emplace_back(std::make_unique<fbs::SceneNodeT>()).get();
            if (node_name.size() > 0)
                oscene.node_names[node_name] = node_index;
        } else if (it->attribute("override")) {
            node_index = node_it->second;
            node = oscene->nodes[node_index].get();
        } else {
            std::cerr << "[E] declared node " << node_name << " without override, but it already exists" << std::endl;
            continue;
        }

        if (it->attribute("parent") && *it->attribute("parent").as_string()) {
            std::string_view parent_name = it->attribute("parent").as_string();
            auto parent_it = oscene.node_names.find(parent_name);
            if (node_name.size() > 0 && parent_name == node_name) {
                std::cerr << "[E] node " << node_name << "cannot be its own parent" << std::endl;
                continue;
            }
            if (parent_it == oscene.node_names.end()) {
                std::cerr << "[E] node parent=" << parent_name << " but no node with that name exists" << std::endl;
                continue;
            }

            oscene->nodes[parent_it->second]->children.push_back(node_index);
            std::erase(oscene->roots, node_index);
        } else {
            oscene->roots.push_back(node_index);
        }
        if (it->child("camera") && it->child("camera").text().as_uint(0)) {
            node->camera = it->child("camera").text().as_uint(0);
        }
        if (pugi::xml_node xfm = it->child("transform")) {
            if (xfm.child("matrix")) {
                std::stringstream ss;
                float f;
                std::vector<float> mat;
                for (pugi::xml_node child = xfm.child("matrix").first_child(); child; child = child.next_sibling()) {
                    if (child.type() == pugi::node_cdata || child.type() == pugi::node_pcdata)
                        ss << child.value();
                }
                while (ss >> f)
                    mat.push_back(f);
                if (mat.size() < 16) {
                    std::cerr << "[E] node(" << node_name << ").transform.matrix is malformed" << std::endl;
                    continue;
                }

                node->transform.Set(fbs::Mat4(std::to_array({
                    fbs::Vec4(std::span<const float, 4> { mat.data() + 0, 4 }),
                    fbs::Vec4(std::span<const float, 4> { mat.data() + 4, 4 }),
                    fbs::Vec4(std::span<const float, 4> { mat.data() + 8, 4 }),
                    fbs::Vec4(std::span<const float, 4> { mat.data() + 12, 4 }),
                })));
            } else {
                std::array<float, 4> rotation = { 0, 0, 0, 1 };
                std::array<float, 3> translation = { 0, 0, 0 }, scale = { 1, 1, 1 };
                if (xfm.child("translation")) {
                    if (sscanf(xfm.child("translation").text().get(), "%f %f %f", &translation[0], &translation[1], &translation[2]) < 3) {
                        std::cerr << "[E] node(" << node_name << ").transform.translation has too few elements" << std::endl;
                        continue;
                    }
                }
                if (xfm.child("rotation")) {
                    if (sscanf(xfm.child("rotation").text().get(), "%f %f %f %f", &rotation[0], &rotation[1], &rotation[2], &rotation[3]) < 4) {
                        std::cerr << "[E] node(" << node_name << ").transform.rotation has too few elements" << std::endl;
                        continue;
                    }
                }
                if (xfm.child("scale")) {
                    if (sscanf(xfm.child("scale").text().get(), "%f %f %f", &scale[0], &scale[1], &scale[2]) < 3) {
                        std::cerr << "[E] node(" << node_name << ").transform.scale has too few elements" << std::endl;
                        continue;
                    }
                }
                node->transform.Set(fbs::TRS(fbs::Vec4(rotation), fbs::Vec3(translation), fbs::Vec3(scale)));
            }
        } else {
            std::array<float, 4> rotation = { 0, 0, 0, 1 };
            std::array<float, 3> translation = { 0, 0, 0 }, scale = { 1, 1, 1 };
            node->transform.Set(fbs::TRS(fbs::Vec4(rotation), fbs::Vec3(translation), fbs::Vec3(scale)));
        }
    }
}

int main(int argc, char** argv)
{
    fs::path infile, outparam;
    for (int i = 0; ++i < argc;) {
        if (strcmp(argv[i], "-o") == 0) {
            outparam = argv[++i];
        } else if (strcmp(argv[i], "--max-uvs") == 0) {
            if ((max_uv_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-uvs: " << argv[i] << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--max-colors") == 0) {
            if ((max_color_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-colors: " << argv[i] << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--max-joints") == 0) {
            if ((max_joint_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-joints: " << argv[i] << std::endl;
                return 1;
            }
            max_joint_channels = (max_joint_channels + 3) & (~3);
        } else if (argv[i][0] == '-') {
            int j = 0;
            while (argv[i][++j]) {
                switch (argv[i][j]) {
                case 's':
                    image_enable_uastc = true;
                    break;
                case 0:
                    break;
                default:
                    std::cerr << "unknown option -" << argv[i][j] << std::endl;
                    return 1;
                }
            }
        } else {
            infile = argv[i];
        }
    }

    if (infile.empty() || outparam.empty())
        return usage(*argv);
    outparam = fs::absolute(outparam);
    if (fs::is_directory(outparam.parent_path()) == false)
        return usage(*argv, "OUTFILE: file not found");
    if (fs::is_regular_file(infile) == false)
        return usage(*argv, "INFILE: file not found");

    pugi::xml_document maindoc;
    pugi::xml_parse_result result = maindoc.load_file(fs::absolute(infile).c_str());
    if (!result)
        return usage(*argv, result.description());

    fastgltf::Parser gltf_parser;
    OutputScene oscene(outparam);

    pugi::xml_node scene_root = maindoc.root().child("scene");
    for (auto it = scene_root.children().begin(); it != scene_root.children().end(); ++it) {
        if (strcmp(it->name(), "glTF") == 0 || strcmp(it->name(), "gltf") == 0) {
            fastgltf::Options options = fastgltf::Options::DontRequireValidAssetMember
                | fastgltf::Options::GenerateMeshIndices
                | fastgltf::Options::LoadExternalBuffers
                | fastgltf::Options::LoadExternalImages;
            std::string_view relative_path = it->attribute("source").as_string();
            if (relative_path.empty())
                continue;

            fs::path gltf_path = fs::weakly_canonical(infile.parent_path() / relative_path);
            auto data = fastgltf::GltfDataBuffer::FromPath(gltf_path);
            if (data.error() != fastgltf::Error::None) {
                std::cerr << "[E] " << gltf_path << ": invalid glTF stream" << std::endl;
                continue;
            }
            auto asset = gltf_parser.loadGltf(data.get(), gltf_path.parent_path(), options);
            if (asset.error() != fastgltf::Error::None) {
                std::cerr << "[E] " << gltf_path << ": invalid glTF data" << std::endl;
                continue;
            }

            std::cerr << "[I] loading glTF " << gltf_path.string() << std::endl;
            generate_missing_attributes(asset.get());
            std::cerr << "[I] generated missing normals/tangents" << std::endl;
            load_gltf(oscene, asset.get());
        } else if (strcmp(it->name(), "graph") == 0) {
            load_scene_extensions(oscene, *it);
        }
    }

    oscene.finish();
    return 0;
}
