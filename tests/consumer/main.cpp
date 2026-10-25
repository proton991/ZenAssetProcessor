#include <ZenAsset/Format/Scene.h>
#include <ZenAsset/Format/Version.h>
#include <ZenAsset/Reader/Package.h>
#include <ZenAsset/Reader/Scene.h>
#include <ZenAsset/Reader/Image.h>
#include <array>
#include <iostream>

int main(int argc, char** argv)
{
    int result = 1;
    if (argc == 1)
    {
        const zenasset::PackageResult opened = zenasset::OpenPackage("missing.zenasset");
        std::cout << "ZenAsset " << zenasset::LibraryVersion() << '\n';
        result = opened.status.code == zenasset::StatusCode::SourceUnavailable ? 0 : 1;
    }
    else if (argc == 2)
    {
        const zenasset::SceneReadResult decoded = zenasset::ReadScene(argv[1]);
        if (decoded.status.Succeeded() && decoded.scene)
        {
            std::cout << "Read " << decoded.scene->nodes.size() << " nodes and " << decoded.scene->vertices.size()
                      << " vertices from installed Reader\n";
            result = 0;
        }
        else
        {
            std::cerr << decoded.status.message << '\n';
        }
    }
    const std::array<std::byte, 16>    block{std::byte{64}};
    const zenasset::ImageMipView       mip{zenasset::PixelFormat::BC7Srgb, 1, 1, block};
    const zenasset::ImagePreviewResult preview = zenasset::MakeImagePreview(mip);
    if (!preview.status.Succeeded() || preview.width != 1 || preview.height != 1 || preview.pixels.size() != 4)
    {
        std::cerr << "Installed Reader BC7 preview failed\n";
        result = 1;
    }
    const std::array<uint8_t, 16> normal{128, 128, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0};
    for (zenasset::PixelFormat format : {zenasset::PixelFormat::BC4Unorm, zenasset::PixelFormat::BC5Unorm})
    {
        const bool                         isNormal = format == zenasset::PixelFormat::BC5Unorm;
        const zenasset::ImageMipView       view{format, 1, 1, std::as_bytes(std::span(normal)).first(isNormal ? 16 : 8),
                                                isNormal ? zenasset::TextureInterpretation::NormalXYPositiveZ
                                                         : zenasset::TextureInterpretation::Default};
        const zenasset::ImagePreviewResult decoded = zenasset::MakeImagePreview(view);
        if (!decoded.status.Succeeded() || decoded.pixels != std::vector<uint8_t>{128, 128, uint8_t(isNormal ? 255 : 128), 255})
        {
            std::cerr << "Installed Reader BC4/BC5 preview failed\n";
            result = 1;
        }
    }
    return result;
}
