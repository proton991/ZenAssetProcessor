#include "Processing/ArrayPacking.h"
#include "Processing/TextureImage.h"
#include "ZenAsset/Format/Validation.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Reader/Scene.h"
#include "ZenAsset/Reader/Image.h"
#include "Format/PackageLayout.h"
#include "SceneValueComparison.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;
uint32_t           checks = 0;
constexpr uint64_t Budget = 64ull << 20;

bool Check(bool valid, const char* message)
{
    ++checks;
    if (!valid)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return valid;
}

std::shared_ptr<ImagePixels> Image(uint32_t width, uint32_t height, PixelFormat format, uint32_t seed, bool partial)
{
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width                       = width;
    pixels->height                      = height;
    pixels->format                      = format;
    PixelFormatInfo info;
    GetPixelFormatInfo(format, info);
    bool done = false;
    while (!done)
    {
        uint64_t bytes = 0;
        GetImageLevelByteSize(format, width, height, bytes);
        std::vector<uint8_t> level(static_cast<size_t>(bytes));
        for (size_t index = 0; index < level.size(); ++index)
        {
            level[index] = static_cast<uint8_t>(seed * 47 + index * 19 + pixels->mips.size() * 61);
            if (info.compressed && index % info.bytesPerBlock == 0)
            {
                level[index] = 0x40;
            }
        }
        pixels->mips.push_back(std::move(level));
        done   = (width == 1 && height == 1) || (partial && pixels->mips.size() == 2);
        width  = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    pixels->bytes = pixels->mips[0];
    return pixels;
}

bool Verify(const std::vector<AtlasTileInput>& inputs, const ArrayPackResult& packed, const ArrayPackingOptions& options)
{
    bool valid = Check(packed.status.Succeeded() && packed.tiles.size() == inputs.size(), "Every array input has a mapping");
    uint64_t owned = packed.tiles.size() * sizeof(ArrayTileLayer) + packed.pages.size() * sizeof(AtlasPage);
    std::vector<std::vector<uint8_t>> occupied;
    for (const AtlasPage& page : packed.pages)
    {
        const ImagePixels& pixels = *page.pixels;
        valid &= Check(pixels.arrayLayers >= 2 && pixels.arrayLayers <= options.maxLayers, "Array layer count is bounded");
        valid &= Check(!pixels.mips.empty() && pixels.bytes == pixels.mips[0], "Array page has explicit consistent mip zero");
        occupied.emplace_back(pixels.arrayLayers, 0);
        owned += sizeof(ImagePixels) + pixels.mips.size() * sizeof(std::vector<uint8_t>) + pixels.bytes.size();
        for (const std::vector<uint8_t>& mip : pixels.mips)
        {
            owned += mip.size();
        }
    }
    for (size_t index = 0; index < packed.tiles.size(); ++index)
    {
        const ArrayTileLayer& tile = packed.tiles[index];
        valid &= Check(index == 0 || packed.tiles[index - 1].id < tile.id, "Mappings are sorted by unique stable ID");
        const AtlasTileInput* input = nullptr;
        for (const AtlasTileInput& candidate : inputs)
        {
            if (candidate.id == tile.id)
            {
                input = &candidate;
            }
        }
        valid &= Check(input != nullptr, "Mapping preserves an input ID");
        if (input && tile.page != UINT32_MAX)
        {
            valid &= Check(tile.page < packed.pages.size(), "Mapping references a valid page");
            if (tile.page < packed.pages.size())
            {
                const AtlasPage&   page   = packed.pages[tile.page];
                const ImagePixels& pixels = *page.pixels;
                valid &=
                    Check(tile.layer < pixels.arrayLayers && page.group == input->group, "Layer and group match the input");
                valid &=
                    Check(pixels.width == input->pixels->width && pixels.height == input->pixels->height
                              && pixels.format == input->pixels->format && pixels.mips.size() == input->pixels->mips.size(),
                          "Array groups preserve exact extent, format and authored mip count");
                if (tile.layer < pixels.arrayLayers)
                {
                    valid &= Check(occupied[tile.page][tile.layer]++ == 0, "No two input IDs occupy one layer");
                }
                for (uint32_t level = 0; level < input->pixels->mips.size(); ++level)
                {
                    std::span<const uint8_t> view;
                    valid &= Check(GetImageLayerMipBytes(pixels, tile.layer, level, view)
                                       && std::equal(view.begin(), view.end(), input->pixels->mips[level].begin(),
                                                     input->pixels->mips[level].end()),
                                   "Every stored layer/mip exactly preserves the original bytes");
                }
            }
        }
    }
    for (const std::vector<uint8_t>& page : occupied)
    {
        valid &=
            Check(std::all_of(page.begin(), page.end(), [](uint8_t count) { return count == 1; }), "No unused array layers");
    }
    valid &= Check(owned == packed.ownedBytes, "Retained requested-storage accounting equals all result allocations");
    return valid;
}

bool TestPacking()
{
    std::vector<AtlasTileInput>   inputs;
    uint32_t                      id = 0;
    const std::array<uint32_t, 2> dimensions[]{{1, 1}, {5, 7}, {17, 1}, {32, 32}};
    for (PixelFormat format : {PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Srgb, PixelFormat::BC4Unorm, PixelFormat::BC5Unorm,
                               PixelFormat::BC7Unorm, PixelFormat::BC7Srgb})
    {
        for (const std::array<uint32_t, 2>& size : dimensions)
        {
            for (uint32_t group = 0; group < 2; ++group)
            {
                for (uint32_t layer = 0; layer < 3; ++layer)
                {
                    inputs.push_back({id++, group, Image(size[0], size[1], format, id, group != 0)});
                }
            }
        }
    }
    ArrayPackingOptions options;
    options.maxLayers                = 3;
    uint64_t              remaining  = Budget;
    const ArrayPackResult first      = PackArrayPages(inputs, options, remaining);
    bool                  valid      = Verify(inputs, first, options);
    valid                           &= Check(first.pages.size() == 48 && Budget - remaining == first.ownedBytes,
                                             "Exact groups form 48 pages with scratch refunded");
    std::reverse(inputs.begin(), inputs.end());
    remaining                     = Budget;
    const ArrayPackResult reverse = PackArrayPages(inputs, options, remaining);
    valid &= Check(reverse.status.Succeeded() && reverse.tiles == first.tiles && reverse.pages.size() == first.pages.size(),
                   "Reversed inputs preserve page and layer assignment");
    for (size_t index = 0; index < std::min(first.pages.size(), reverse.pages.size()); ++index)
    {
        valid &= Check(scene_test::Same(*first.pages[index].pixels, *reverse.pages[index].pixels),
                       "Permutation preserves every page byte");
    }
    options.maxLayers            = 2;
    remaining                    = Budget;
    const ArrayPackResult split  = PackArrayPages(inputs, options, remaining);
    valid                       &= Verify(inputs, split, options);
    size_t standalone            = 0;
    for (const ArrayTileLayer& tile : split.tiles)
    {
        standalone += tile.page == UINT32_MAX;
    }
    valid     &= Check(standalone == 48 && split.pages.size() == 48, "Singleton remainders retain standalone images");
    remaining  = Budget;
    const ArrayPackResult empty = PackArrayPages({}, options, remaining);
    valid &= Check(empty.status.Succeeded() && empty.tiles.empty() && empty.pages.empty() && remaining == Budget,
                   "Empty input succeeds without consuming budget");
    return valid;
}

bool Failure(const std::vector<AtlasTileInput>& inputs, const ArrayPackingOptions& options, uint64_t budget, StatusCode code)
{
    uint64_t              remaining = budget;
    const ArrayPackResult packed    = PackArrayPages(inputs, options, remaining);
    return Check(packed.status.code == code && packed.pages.empty() && packed.tiles.empty() && packed.ownedBytes == 0
                     && remaining == budget,
                 "Failed array packing publishes nothing and refunds all charges");
}

struct PollState
{
    uint32_t count{0};
    uint32_t stop;
};

bool CancelAt(void* user)
{
    PollState& state = *static_cast<PollState*>(user);
    return ++state.count >= state.stop;
}

bool TestFailures()
{
    std::shared_ptr<ImagePixels> pixels = Image(5, 7, PixelFormat::BC7Unorm, 1, false);
    std::vector<AtlasTileInput>  inputs{{9, 0, pixels}, {3, 0, pixels}, {17, 0, pixels}, {23, 0, pixels}};
    ArrayPackingOptions          options;
    bool                         valid = true;
    for (uint64_t budget = 0; budget < 1600; budget += 17)
    {
        uint64_t              remaining = budget;
        const ArrayPackResult packed    = PackArrayPages(inputs, options, remaining);
        valid &= Check(packed.status.Succeeded() ? remaining + packed.ownedBytes == budget
                                                 : packed.status.code == StatusCode::ResourceLimit && packed.pages.empty()
                                                       && packed.tiles.empty() && packed.ownedBytes == 0 && remaining == budget,
                       "Budget boundaries retain exact accounting or fail atomically");
    }
    options.maxLayers  = 2;
    options.maxPages   = 1;
    valid             &= Failure(inputs, options, Budget, StatusCode::ResourceLimit);
    options            = {};
    inputs[1].id       = inputs[0].id;
    valid             &= Failure(inputs, options, Budget, StatusCode::InvalidArgument);
    inputs[1].id       = 3;
    pixels->mips.back().push_back(0);
    valid &= Failure(inputs, options, Budget, StatusCode::InvalidData);
    pixels->mips.back().pop_back();
    pixels->mips.back()[0]  = 0;
    valid                  &= Failure(inputs, options, Budget, StatusCode::InvalidData);
    pixels->mips.back()[0]  = 0x40;
    pixels->arrayLayers     = 2;
    valid                  &= Failure(inputs, options, Budget, StatusCode::InvalidData);
    pixels->arrayLayers     = 1;
    options.maxDimension    = 4;
    valid                  &= Failure(inputs, options, Budget, StatusCode::ResourceLimit);
    options                 = {};
    options.maxLayers       = 1;
    valid                  &= Failure(inputs, options, Budget, StatusCode::InvalidArgument);
    options                 = {};
    for (uint32_t stop = 1; stop < 140; ++stop)
    {
        PollState             poll{0, stop};
        Cancellation          cancellation(CancelAt, &poll);
        uint64_t              remaining = Budget;
        const ArrayPackResult packed    = PackArrayPages(inputs, options, remaining, &cancellation);
        valid &= Check(packed.status.Succeeded() ? remaining + packed.ownedBytes == Budget
                                                 : packed.status.code == StatusCode::Cancelled && packed.pages.empty()
                                                       && packed.tiles.empty() && packed.ownedBytes == 0 && remaining == Budget,
                       "Cancellation at planning/copy boundaries exposes no partial pages and refunds storage");
    }
    pixels->mips.clear();
    valid &= Failure(inputs, options, Budget, StatusCode::InvalidData);
    return valid;
}

Scene SceneFor(const std::shared_ptr<const ImagePixels>& pixels)
{
    Scene scene;
    scene.images.push_back({"page", pixels});
    for (uint32_t layer = 0; layer < pixels->arrayLayers; ++layer)
    {
        TextureAsset texture;
        texture.name       = "layer " + std::to_string(layer);
        texture.image      = ImageId{0};
        texture.format     = pixels->format;
        texture.arrayLayer = layer;
        texture.interpretation =
            pixels->format == PixelFormat::BC5Unorm ? TextureInterpretation::NormalXYPositiveZ : TextureInterpretation::Default;
        scene.textures.push_back(texture);
    }
    return scene;
}

bool CorruptArrayField(const std::filesystem::path& path, ResourceType type, uint32_t value, uint32_t forcedVersion = 0)
{
    std::ifstream        input(path, std::ios::binary | std::ios::ate);
    const std::streamoff length = input.tellg();
    bool                 valid  = input && length > 64;
    std::vector<uint8_t> bytes;
    if (valid)
    {
        bytes.resize(static_cast<size_t>(length));
        input.seekg(0);
        valid = bool(input.read(reinterpret_cast<char*>(bytes.data()), length));
    }
    input.close();
    bool changed = false;
    if (valid)
    {
        const uint64_t count = Read64(bytes.data() + 32);
        for (uint64_t index = 0; index < count; ++index)
        {
            uint8_t* entry = bytes.data() + HeaderBytes + index * DirectoryEntryBytes;
            if (!changed && Read32(entry + 8) == uint32_t(type) && (type != ResourceType::Texture || Read32(entry + 12) == 3))
            {
                const uint64_t offset = Read64(entry + 24);
                const uint64_t stored = Read64(entry + 32);
                if (forcedVersion)
                {
                    Write32(entry + 12, forcedVersion);
                }
                else
                {
                    Write32(bytes.data() + offset + stored - 4, value);
                    Write64(entry + 48, HashBytes(bytes.data() + offset, static_cast<size_t>(stored)));
                }
                changed = true;
            }
        }
        Write64(bytes.data() + 48, HashBytes(bytes.data() + HeaderBytes, static_cast<size_t>(count * DirectoryEntryBytes)));
        Write64(bytes.data() + 56, HashBytes(bytes.data(), 56));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        valid = changed
             && bool(output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    }
    if (valid && !forcedVersion)
    {
        const PackageResult package  = OpenPackage(path.string());
        valid                       &= Check(package.status.Succeeded() && package.package,
                                             "Corrupted field fixture retains valid package framing/checksums");
        if (package.package)
        {
            for (const ResourceRecord& record : package.package->Resources())
            {
                valid &= Check(package.package->Read(record.id).status.Succeeded(),
                               "Every corrupted-fixture resource checksum remains valid");
            }
        }
    }
    const SceneReadResult read = valid ? ReadScene(path.string()) : SceneReadResult{};
    valid &= Check(valid && !read.scene
                       && (read.status.code == StatusCode::InvalidData || read.status.code == StatusCode::UnsupportedVersion),
                   "Checksummed invalid array layer/count/version must fail without exposing a scene");
    return valid;
}

bool TestPackages(const std::filesystem::path& path)
{
    bool valid = true;
    for (PixelFormat format : {PixelFormat::RGBA8Unorm, PixelFormat::RGBA8Srgb, PixelFormat::BC4Unorm, PixelFormat::BC5Unorm,
                               PixelFormat::BC7Unorm, PixelFormat::BC7Srgb})
    {
        std::vector<AtlasTileInput> inputs{
            {9, 0, Image(5, 7, format, 0, true)}, {3, 0, Image(5, 7, format, 1, true)}, {5, 0, Image(5, 7, format, 2, true)}};
        uint64_t              remaining = Budget;
        const ArrayPackResult packed    = PackArrayPages(inputs, {}, remaining);
        valid &= Check(packed.status.Succeeded() && packed.pages.size() == 1, "Package fixture packs");
        if (packed.status.Succeeded() && packed.pages.size() == 1)
        {
            Scene scene  = SceneFor(packed.pages[0].pixels);
            valid       &= Check(ValidateSceneAsset(scene), "Layered image and texture references are valid");
            for (TransportCodec codec : {TransportCodec::None, TransportCodec::Zstandard})
            {
                SceneWriteOptions options;
                options.compression.transport = codec;
                const Status          written = WriteScene(path.string(), scene, options);
                const SceneReadResult read    = ReadScene(path.string());
                valid &= Check(written.Succeeded() && read.scene && scene_test::Same(scene, *read.scene),
                               "Every array field and byte round-trips through raw/Zstandard packages");
                if (read.scene)
                {
                    for (const TextureAsset& texture : read.scene->textures)
                    {
                        std::span<const uint8_t> layer;
                        const ImagePixels&       pixels  = *read.scene->images[texture.image.value].pixels;
                        valid                           &= Check(GetImageLayerMipBytes(pixels, texture.arrayLayer, 1, layer),
                                                                 "Decoded texture selects one mip/layer");
                        const ImagePreviewResult preview =
                            MakeImagePreview({texture.format, 2, 3, std::as_bytes(layer), texture.interpretation});
                        valid &= Check(preview.status.Succeeded() && preview.pixels.size() == 24,
                                       "Bounded preview accepts one logical layer");
                    }
                }
            }
            scene.textures[0].arrayLayer          = 3;
            valid                                &= Check(!ValidateSceneAsset(scene), "Out-of-range texture layer is rejected");
            scene.textures[0].arrayLayer          = 0;
            std::shared_ptr<ImagePixels> invalid  = std::make_shared<ImagePixels>(*scene.images[0].pixels);
            scene.images[0].pixels                = invalid;
            invalid->arrayLayers                  = 0;
            valid                                &= Check(!ValidateSceneAsset(scene), "Zero image layers are rejected");
            invalid->arrayLayers                  = 3;
            invalid->mips.clear();
            valid         &= Check(!ValidateSceneAsset(scene), "Array pages cannot request implicit mip generation");
            invalid->mips  = packed.pages[0].pixels->mips;
            invalid->mips[1].pop_back();
            valid                  &= Check(!ValidateSceneAsset(scene), "Truncated last layer is rejected");
            scene.images[0].pixels  = packed.pages[0].pixels;
            for (uint32_t mutation = 0; mutation < 4; ++mutation)
            {
                valid &= Check(WriteScene(path.string(), scene).Succeeded(), "Valid array fixture writes before corruption");
                valid &= CorruptArrayField(path, mutation == 1 ? ResourceType::Texture : ResourceType::Image,
                                           mutation == 0   ? 0u
                                           : mutation == 1 ? 3u
                                                           : UINT32_MAX,
                                           mutation == 3 ? 99u : 0u);
            }
            TextureSettings settings;
            settings.generateMipmaps          = true;
            remaining                         = Budget;
            const TextureImageResult retained = ProcessTextureImage(packed.pages[0].pixels, {}, settings, remaining);
            valid &= Check(retained.status.Succeeded() && retained.pixels == packed.pages[0].pixels && !retained.reason.empty(),
                           "Recooking retains finalized array pages instead of treating all layers as one 2D image");
        }
    }
    uint64_t size = 1;
    valid &= Check(!GetImageArrayLevelByteSize(PixelFormat::RGBA8Unorm, UINT32_MAX, UINT32_MAX, UINT32_MAX, size) && size == 0,
                   "Array byte-size overflow is rejected");
    const std::shared_ptr<ImagePixels> pixels = Image(1, 1, PixelFormat::RGBA8Unorm, 0, false);
    std::span<const uint8_t>           view   = pixels->bytes;
    valid         &= Check(!GetImageLayerMipBytes(*pixels, 0, 32, view) && view.empty(), "Invalid shifts clear the layer view");
    pixels->width  = 0;
    valid         &= Check(!GetImageLayerMipBytes(*pixels, 0, 0, view), "Layer view rejects zero extent");
    return valid;
}
} // namespace

int main()
{
    const int64_t               tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::current_path() / ("array-packing-" + std::to_string(tick) + ".zenasset");
    bool valid  = TestPacking();
    valid      &= TestFailures();
    valid      &= TestPackages(path);
    std::printf("%u array packing/format checks\n", checks);
    return valid ? 0 : 1;
}
