#pragma once
#include <ZenAsset/Format/Scene.h>

// Original, bounded texture/HDR workload. No external artwork or source file.
inline std::shared_ptr<zenasset::Scene> MakeStageFixture()
{
    using namespace zenasset;
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->name                  = "original-stage-textures-v1";
    scene->sourcePath            = "original-stage-textures-v1";
    scene->sourceDocument        = "{\"generator\":\"original stage fixture v1\",\"images\":3,\"extent\":64,"
                                   "\"color\":\"R=64+x/4;G=128+y/4;B=192;A=255\","
                                   "\"normal\":\"R=128+x/16;G=128+y/16;B=255;A=255\","
                                   "\"occlusion\":\"R=64+x/4;G=B=0;A=255\","
                                   "\"environment\":\"16x16,3 authored mips,6 faces;RGBA=(4+face,2,16,1)\"}";
    scene->vertexTexCoordSets    = {0};
    scene->vertices.resize(3);
    scene->vertices[0].pos = Vec4(0, 0, 0, 1);
    scene->vertices[1].pos = Vec4(1, 0, 0, 1);
    scene->vertices[2].pos = Vec4(0, 1, 0, 1);
    scene->vertices[0].uv0 = Vec2(0, 0);
    scene->vertices[1].uv0 = Vec2(1, 0);
    scene->vertices[2].uv0 = Vec2(0, 1);
    for (Vertex& vertex : scene->vertices)
    {
        vertex.normal  = Vec4(0, 0, 1, 0);
        vertex.tangent = Vec4(1, 0, 0, 1);
        scene->vertexTexCoords.push_back({vertex.uv0});
    }
    scene->indices = {0, 1, 2};
    scene->nodes.resize(1);
    scene->nodes[0].mesh = MeshId{0};
    scene->roots         = {NodeId{0}};
    scene->meshes.resize(1);
    MeshPrimitiveAsset primitive;
    primitive.vertexCount = primitive.indexCount = 3;
    primitive.material                           = MaterialId{0};
    primitive.bounds                             = Bounds(Vec3(0), Vec3(1, 1, 0));
    scene->meshes[0].primitives.push_back(primitive);
    SamplerAsset sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = TextureFilter::Linear;
    sampler.wrapS = sampler.wrapT = SamplerAddressMode::ClampToEdge;
    sampler.useMipmaps            = true;
    scene->samplers.push_back(sampler);
    for (uint32_t image = 0; image < 3; ++image)
    {
        std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
        pixels->width = pixels->height = 64;
        pixels->format                 = image == 0 ? PixelFormat::RGBA8Srgb : PixelFormat::RGBA8Unorm;
        pixels->bytes.resize(64 * 64 * 4);
        for (uint32_t y = 0; y < 64; ++y)
        {
            for (uint32_t x = 0; x < 64; ++x)
            {
                uint8_t* pixel = pixels->bytes.data() + (y * 64 + x) * 4;
                pixel[0]       = uint8_t(image == 1 ? 128 + x / 16 : 64 + x / 4);
                pixel[1]       = uint8_t(image == 0 ? 128 + y / 4 : image == 1 ? 128 + y / 16 : 0);
                pixel[2]       = uint8_t(image == 0 ? 192 : image == 1 ? 255 : 0);
                pixel[3]       = 255;
            }
        }
        scene->images.push_back({"original-image-" + std::to_string(image), pixels});
        TextureAsset texture;
        texture.image   = ImageId{image};
        texture.sampler = SamplerId{0};
        texture.format  = pixels->format;
        scene->textures.push_back(texture);
    }
    MaterialAsset material;
    material.m_pBaseColorTexture = TextureId{0};
    material.m_pNormalTexture    = TextureId{1};
    material.m_pOcclusionTexture = TextureId{2};
    scene->materials.push_back(material);
    std::shared_ptr<EnvironmentCubePixels> cube = std::make_shared<EnvironmentCubePixels>();
    cube->size                                  = 16;
    cube->mipLevels                             = 3;
    for (uint32_t level = 0; level < cube->mipLevels; ++level)
    {
        const uint32_t extent = cube->size >> level;
        for (uint32_t face = 0; face < 6; ++face)
        {
            cube->rgba32FloatMipFaces.emplace_back(size_t(extent) * extent, Vec4(4 + float(face), 2, 16, 1));
        }
    }
    scene->imageBasedLight = 0;
    scene->imageBasedLights.resize(1);
    scene->imageBasedLights[0].irradianceCoefficients.assign(9, Vec3(0.5f, 0.25f, 0.125f));
    scene->imageBasedLights[0].specular = cube;
    return scene;
}
