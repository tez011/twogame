#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include "gltf2tg.h"

namespace fs = std::filesystem;

int usage(const char* argv0, const std::string& extra = "")
{
    std::cerr << "usage: " << argv0 << " [-s] -o OUTFILE INFILE" << std::endl
              << "\t-s: enable UASTC compression of image mip levels" << std::endl
              << "\tINFILE must be a valid glTF file." << std::endl;
    if (extra.empty() == false)
        std::cerr << std::endl
                  << '\t' << extra << std::endl;
    return 1;
}

int main(int argc, char** argv)
{
    fs::path infile, outparam;
    bool enable_uastc = false;

    for (int i = 0; ++i < argc;) {
        if (strcmp(argv[i], "-o") == 0) {
            outparam = argv[++i];
        } else if (argv[i][0] == '-') {
            int j = 0;
            while (argv[i][++j]) {
                switch (argv[i][j]) {
                case 's':
                    enable_uastc = true;
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

    assert(volkInitialize() == VK_SUCCESS);
    if (infile.empty() || outparam.empty())
        return usage(*argv);

    outparam = fs::absolute(outparam);
    if (fs::is_directory(outparam.parent_path()) == false)
        return usage(*argv, "OUTFILE: file not found");
    if (fs::is_regular_file(infile) == false)
        return usage(*argv, "INFILE: file not found");

    // buffer formats: {position+uv} {normal+tangent+color} {index} in three buffers
    // RGBA16F for displacements, later

    fs::path out_assets_path = outparam, out_data_path = outparam;
    out_assets_path.replace_extension(".asset");
    out_data_path.replace_extension(".bin");

    char zero[16];
    std::vector<char> scratch(102040);
    std::ifstream in_data(infile.parent_path() / "Duck0.bin");
    std::ofstream out_data(out_data_path, std::ios::binary);
    memset(zero, 0, sizeof(zero));

    // index
    in_data.seekg(76768, std::ios_base::beg);
    in_data.read(scratch.data(), 25272);
    out_data.write(scratch.data(), 25272);
    out_data.write(zero, 8);
    // position
    in_data.seekg(28788, std::ios_base::beg);
    in_data.read(scratch.data(), 28788);
    // uv0
    in_data.seekg(57576, std::ios_base::beg); // technically redundant but explicitness is good here
    in_data.read(scratch.data() + 28788, 19192);
    for (size_t i = 0; i < 2399; i++) {
        out_data.write(scratch.data() + (i * 12), 12);
        out_data.write(zero, 4);
        out_data.write(scratch.data() + 28788 + (i * 8), 8);
        out_data.write(zero, 8);
    }
    // normal
    in_data.seekg(0, std::ios_base::beg);
    in_data.read(scratch.data(), 28788);
    for (size_t i = 0; i < 2399; i++) {
        out_data.write(scratch.data() + (i * 12), 12);
        out_data.write(zero, 4);
        out_data.write(zero, 16);
        out_data.write(zero, 16);
    }
    in_data.close();
    out_data.close();

    // image
    ImageGenerator makeimage;
    makeimage.set_uastc(enable_uastc);
    std::vector<fs::path> out_image_path(1);
    std::vector<size_t> out_image_size(out_image_path.size());
    std::fill(out_image_path.begin(), out_image_path.end(), outparam);
    for (size_t i = 0; i < out_image_path.size(); i++) {
        char ext[16];
        snprintf(ext, 16, ".i%zu.ktx2", i);
        out_image_path[i].replace_extension(ext);

        std::ofstream out_image(out_image_path[i]);
        makeimage.generate(out_image, infile.parent_path() / "DuckCM.png");
        out_image_size[i] = out_image.tellp();
    }

    return 0;
}
