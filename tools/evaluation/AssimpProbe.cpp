#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/version.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace
{
void String(std::string_view value)
{
    std::cout << '"';
    for (unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            std::cout << '\\' << character;
        }
        else if (character < 32)
        {
            std::cout << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
        }
        else
        {
            std::cout << character;
        }
    }
    std::cout << '"';
}

void Number(ai_real value)
{
    if (std::isfinite(value))
    {
        std::cout << std::setprecision(9) << value;
    }
    else
    {
        std::cout << "null";
    }
}

void Vector(const aiVector3D& value)
{
    std::cout << '[';
    Number(value.x);
    std::cout << ',';
    Number(value.y);
    std::cout << ',';
    Number(value.z);
    std::cout << ']';
}

void Mesh(const aiMesh& mesh)
{
    std::cout << "{\"name\":";
    String(mesh.mName.C_Str());
    std::cout << ",\"vertices\":" << mesh.mNumVertices << ",\"faces\":" << mesh.mNumFaces
              << ",\"primitive_types\":" << mesh.mPrimitiveTypes << ",\"normals\":" << mesh.HasNormals()
              << ",\"colors\":" << mesh.HasVertexColors(0) << ",\"uv_channels\":" << mesh.GetNumUVChannels()
              << ",\"bones\":" << mesh.mNumBones << ",\"morphs\":" << mesh.mNumAnimMeshes << ",\"samples\":[";
    // Keep large upstream fixtures bounded while exposing exact small-fixture data.
    for (unsigned index = 0; index < mesh.mNumVertices && index < 4; ++index)
    {
        std::cout << (index == 0 ? "" : ",") << "{\"position\":";
        Vector(mesh.mVertices[index]);
        if (mesh.HasNormals())
        {
            std::cout << ",\"normal\":";
            Vector(mesh.mNormals[index]);
        }
        if (mesh.HasTextureCoords(0))
        {
            std::cout << ",\"uv\":";
            Vector(mesh.mTextureCoords[0][index]);
        }
        if (mesh.HasVertexColors(0))
        {
            const aiColor4D& color = mesh.mColors[0][index];
            std::cout << ",\"color\":[";
            Number(color.r);
            std::cout << ',';
            Number(color.g);
            std::cout << ',';
            Number(color.b);
            std::cout << ',';
            Number(color.a);
            std::cout << ']';
        }
        std::cout << '}';
    }
    std::cout << "]}";
}
} // namespace

int main(int count, char** values)
{
    bool valid = count == 3 && (std::string_view(values[2]) == "raw" || std::string_view(values[2]) == "triangulate");
    if (valid)
    {
        Assimp::Importer importer;
        const unsigned   flags =
            aiProcess_ValidateDataStructure | (std::string_view(values[2]) == "triangulate" ? aiProcess_Triangulate : 0u);
        const aiScene* scene = importer.ReadFile(values[1], flags);
        valid                = scene != nullptr;
        std::cout << "{\"version\":\"" << aiGetVersionMajor() << '.' << aiGetVersionMinor() << '.' << aiGetVersionPatch()
                  << "\",\"accepted\":" << (valid ? "true" : "false") << ",\"error\":";
        String(importer.GetErrorString());
        if (valid)
        {
            std::cout << ",\"meshes\":" << scene->mNumMeshes << ",\"materials\":" << scene->mNumMaterials
                      << ",\"animations\":" << scene->mNumAnimations << ",\"textures\":" << scene->mNumTextures
                      << ",\"cameras\":" << scene->mNumCameras << ",\"lights\":" << scene->mNumLights
                      << ",\"root_children\":" << scene->mRootNode->mNumChildren << ",\"mesh_data\":[";
            for (unsigned index = 0; index < scene->mNumMeshes; ++index)
            {
                std::cout << (index == 0 ? "" : ",");
                Mesh(*scene->mMeshes[index]);
            }
            std::cout << ']';
        }
        std::cout << "}\n";
    }
    else
    {
        std::cerr << "Usage: AssimpProbe SOURCE raw|triangulate\n";
    }
    return valid ? 0 : 1;
}
