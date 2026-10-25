#include "Importers/ObjImport.h"
#include "Diagnostics/StageMeasurements.h"
#include "Importers/ObjNormal.h"
#include "Importers/AssetPaths.h"
#include "Importers/ImageDecoder.h"
#include "Format/MathOps.h"
#include "Format/FileIO.h"
#include <algorithm>
#include <sstream>

namespace zenasset
{
void ReverseObjNormalGreen(std::vector<uint8_t>& pixels, ImportErrors& errors)
{
    for (size_t first = 0; !errors.Failed() && first < pixels.size(); first += 128 * 1024)
    {
        const size_t last = std::min(pixels.size(), first + 128 * 1024);
        for (size_t channel = first + 1; channel < last; channel += 4)
        {
            pixels[channel] = 255 - pixels[channel];
        }
    }
}
namespace
{
std::filesystem::path ObjPath(ObjImportState& state, const std::string& name)
{
    std::string portable = name;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    std::filesystem::path result;
    const Status          status = detail::MakePath(portable, result);
    if (!status.Succeeded())
    {
        state.errors.Fail("Invalid OBJ dependency path: " + status.message, status.code);
    }
    return result;
}
bool LocalDependency(ObjImportState& state, const std::filesystem::path& candidate)
{
    const std::string absolute = AbsoluteUtf8Path(candidate);
    const std::string source   = AbsoluteUtf8Path(ObjPath(state, state.request.path).parent_path());
    const bool        network  = absolute.starts_with("//") || absolute.starts_with("\\\\");
    const bool        valid    = !network || absolute.starts_with(source + "/");
    if (!valid)
    {
        state.errors.Fail("OBJ dependency refers to an external network/device path", StatusCode::UnsupportedFeature);
    }
    return valid;
}
std::string_view OptionToken(const std::string& text, size_t& position)
{
    const size_t begin = text.find_first_not_of(" \t\r", position);
    position           = begin == std::string::npos ? text.size() : text.find_first_of(" \t\r", begin);
    if (position == std::string::npos)
    {
        position = text.size();
    }
    return begin == std::string::npos ? std::string_view() : std::string_view(text).substr(begin, position - begin);
}
std::string NormalizeMapOptions(ObjImportState& state, std::string text)
{
    size_t position = 0;
    bool   options  = true;
    while (options && !state.errors.Failed() && position < text.size())
    {
        const std::string option(OptionToken(text, position));
        if (option == "-o" || option == "-s" || option == "-t")
        {
            uint32_t count   = 0;
            size_t   end     = position;
            bool     numeric = true;
            while (numeric && count < 3)
            {
                size_t             next = end;
                const std::string  value(OptionToken(text, next));
                std::vector<float> parsed;
                numeric = !value.empty() && ParseObjNumbers(value, parsed) && parsed.size() == 1;
                if (numeric)
                {
                    ++count;
                    end = next;
                }
            }
            if (count == 0)
            {
                state.errors.Fail("MTL texture vector option requires a finite number: " + option);
            }
            else
            {
                std::string padding;
                for (uint32_t component = count; component < 3; ++component)
                {
                    padding += option == "-s" ? " 1" : " 0";
                }
                text.insert(end, padding);
                position = end + padding.size();
            }
        }
        else if (option == "-mm" || option == "-bm" || option == "-boost" || option == "-texres")
        {
            const uint32_t count = option == "-mm" ? 2 : 1;
            for (uint32_t index = 0; index < count; ++index)
            {
                const std::string  value(OptionToken(text, position));
                std::vector<float> parsed;
                if (!ParseObjNumbers(value, parsed) || parsed.size() != 1)
                {
                    state.errors.Fail("Invalid numeric MTL texture option: " + option);
                }
            }
        }
        else if (option == "-clamp" || option == "-blendu" || option == "-blendv" || option == "-imfchan" || option == "-type"
                 || option == "-colorspace")
        {
            const std::string_view value = OptionToken(text, position);
            if (value.empty())
            {
                state.errors.Fail("Missing MTL texture option value: " + option);
            }
            else if ((option == "-clamp" || option == "-blendu" || option == "-blendv") && value != "on" && value != "off")
            {
                state.errors.Fail("Invalid MTL on/off texture option: " + option);
            }
        }
        else
        {
            options = false;
            if (option.starts_with('-'))
            {
                state.errors.Fail("Unsupported MTL texture option: " + option, StatusCode::UnsupportedFeature);
            }
        }
    }
    return text;
}
std::string PrepareMtl(ObjImportState& state, const std::string& source, std::map<std::string, ObjMaterialFields>& fields)
{
    std::istringstream input(source);
    std::string        result;
    std::string        line;
    std::string        material;
    while (!state.errors.Failed() && std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line.resize(comment);
        }
        std::istringstream tokens(line);
        std::string        command;
        tokens >> command;
        std::string rest;
        std::getline(tokens, rest);
        if (command.starts_with("map_") || command == "norm" || command == "bump" || command == "disp" || command == "refl")
        {
            rest = NormalizeMapOptions(state, std::move(rest));
            line = command + rest;
        }
        const bool color =
            command == "Kd" || command == "Ks" || command == "Ka" || command == "Ke" || command == "Tf" || command == "Kt";
        const bool scalar = command == "Ns" || command == "Ni" || command == "d" || command == "Tr" || command == "Pr"
                         || command == "Pm" || command == "illum";
        if (color || scalar)
        {
            std::vector<float> values;
            const bool         numeric = ParseObjNumbers(rest, values);
            if (!numeric || values.size() != (color ? 3 : 1))
            {
                state.errors.Fail("Unsupported or invalid MTL numeric property: " + command);
            }
            else if (scalar
                     && (values[0] < 0
                         || ((command == "d" || command == "Tr" || command == "Pr" || command == "Pm") && values[0] > 1)))
            {
                state.errors.Fail("MTL property is outside its supported range: " + command);
            }
        }
        if (command == "newmtl")
        {
            const size_t first = rest.find_first_not_of(" \t");
            material           = first == std::string::npos ? std::string() : rest.substr(first);
            while (!material.empty() && (material.back() == '\r' || material.back() == ' ' || material.back() == '\t'))
            {
                material.pop_back();
            }
        }
        if (command == "Pm")
        {
            fields[material].metalness = true;
        }
        if (command == "Pr")
        {
            fields[material].roughness = true;
        }
        if (command == "Kd")
        {
            fields[material].diffuse = true;
        }
        if (command == "Ks")
        {
            fields[material].specular = true;
        }
        if (command == "Ke")
        {
            fields[material].emission = true;
        }
        if (command == "illum")
        {
            fields[material].illumination = true;
        }
        // MTL scalar maps without an explicit channel use luminance. tinyobj's
        // generic default 'm' cannot distinguish an authored alpha selection.
        if ((command == "map_d" || command == "map_Ns" || command == "map_Pr" || command == "map_Pm")
            && rest.find("-imfchan") == std::string::npos)
        {
            result += command + " -imfchan l" + rest + "\n";
        }
        else
        {
            result += line + "\n";
        }
    }
    return result;
}
struct ObjMap
{
    MaterialTextureBinding binding;
    char                   channel{'l'};
};
class ObjTextures
{
public:
    explicit ObjTextures(ObjImportState& state) : m_state(state) {}
    ObjMap Read(const std::string&               name,
                const tinyobj::texture_option_t& options,
                const std::filesystem::path&     directory,
                bool                             color,
                bool                             normal = false);
    ObjMap Pack(const ObjMap& first, const ObjMap& second, bool alpha, float exponent, const std::string& name);

private:
    ObjImportState&                m_state;
    std::map<std::string, ImageId> m_images;
};
ObjMap ObjTextures::Read(const std::string&               name,
                         const tinyobj::texture_option_t& options,
                         const std::filesystem::path&     directory,
                         bool                             color,
                         bool                             normal)
{
    ObjMap result;
    result.channel = options.imfchan;
    if (!name.empty() && !m_state.errors.Failed())
    {
        m_state.Charge(1, 3 * sizeof(ImageAsset) + 2 * sizeof(TextureAsset) + sizeof(SamplerAsset) + name.size() * 4);
        const bool unsupported = (!normal && options.bump_multiplier != 1) || options.type != tinyobj::TEXTURE_TYPE_NONE
                              || options.turbulence[0] != 0 || options.turbulence[1] != 0 || options.turbulence[2] != 0
                              || options.brightness != 0 || options.contrast != 1 || options.origin_offset[2] != 0
                              || options.scale[2] != 1 || !options.blendu || !options.blendv || options.sharpness != 1
                              || options.texture_resolution != -1
                              || (options.imfchan != 'r' && options.imfchan != 'g' && options.imfchan != 'b'
                                  && options.imfchan != 'm' && options.imfchan != 'l');
        if (unsupported)
        {
            m_state.errors.Fail("Unsupported MTL texture option on " + name, StatusCode::UnsupportedFeature);
        }
        if (!options.colorspace.empty() && options.colorspace != "sRGB" && options.colorspace != "linear")
        {
            m_state.errors.Fail("Unsupported MTL texture colorspace: " + options.colorspace, StatusCode::UnsupportedFeature);
        }
        if (!color && options.colorspace == "sRGB")
        {
            m_state.errors.Fail("MTL scalar/normal maps require linear samples", StatusCode::UnsupportedFeature);
        }
        std::filesystem::path              path            = ObjPath(m_state, name);
        const std::filesystem::path        sourceDirectory = ObjPath(m_state, m_state.request.path).parent_path();
        std::vector<std::filesystem::path> candidates{path.is_absolute() ? path : directory / path};
        if (!path.is_absolute() && AbsoluteUtf8Path(directory) != AbsoluteUtf8Path(sourceDirectory))
        {
            candidates.push_back(sourceDirectory / path);
        }
        ImageId image;
        for (const std::filesystem::path& candidate : candidates)
        {
            if (!image.IsValid() && !m_state.errors.Failed() && LocalDependency(m_state, candidate))
            {
                const std::string                                    absolute = AbsoluteUtf8Path(candidate);
                const std::map<std::string, ImageId>::const_iterator existing = m_images.find(absolute);
                if (existing != m_images.end())
                {
                    image = existing->second;
                }
                else
                {
                    ZENASSET_MEASURE_SCOPE(SourceImages);
                    std::string encoded;
                    if (m_state.ReadFile(candidate, encoded, true))
                    {
                        std::shared_ptr<ImagePixels> pixels(new ImagePixels);
                        pixels->bytes = DecodeImagePixels(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(),
                                                          pixels->width, pixels->height, m_state.errors, &pixels->mips);
                        if (!m_state.errors.Failed())
                        {
                            image.value = static_cast<uint32_t>(m_state.scene.images.size());
                            m_state.scene.images.push_back({name, pixels});
                            m_images.emplace(absolute, image);
                            m_state.CompleteDependency(candidate);
                        }
                    }
                }
            }
        }
        if (!image.IsValid() && !m_state.errors.Failed())
        {
            m_state.errors.Fail("Missing MTL texture: " + name, StatusCode::SourceUnavailable);
        }
        if (image.IsValid() && !m_state.errors.Failed())
        {
            if (normal)
            {
                const ImagePixels& original = *m_state.scene.images[image.value].pixels;
                uint64_t           bytes    = original.bytes.size();
                for (const std::vector<uint8_t>& mip : original.mips)
                {
                    bytes += mip.size();
                }
                if (m_state.errors.ReserveImageBytes(bytes))
                {
                    std::shared_ptr<ImagePixels> converted(new ImagePixels(original));
                    ReverseObjNormalGreen(converted->bytes, m_state.errors);
                    for (std::vector<uint8_t>& mip : converted->mips)
                    {
                        ReverseObjNormalGreen(mip, m_state.errors);
                    }
                    image.value = static_cast<uint32_t>(m_state.scene.images.size());
                    m_state.scene.images.push_back({name + " (normal V reversal)", converted});
                }
            }
            SamplerAsset sampler;
            sampler.wrapS = sampler.wrapT = options.clamp ? SamplerAddressMode::ClampToEdge : SamplerAddressMode::Repeat;
            sampler.minFilter = sampler.magFilter = TextureFilter::Linear;
            TextureAsset texture;
            texture.name          = name;
            texture.image         = image;
            texture.sampler.value = static_cast<uint32_t>(m_state.scene.samplers.size());
            texture.format = (options.colorspace == "sRGB" || (color && options.colorspace.empty())) ? PixelFormat::RGBA8Srgb
                                                                                                     : PixelFormat::RGBA8Unorm;
            m_state.scene.samplers.push_back(sampler);
            result.binding.texture.value = static_cast<uint32_t>(m_state.scene.textures.size());
            m_state.scene.textures.push_back(texture);
            TextureTransform& transform = result.binding.transform;
            transform.uvScale           = Vec2(options.scale[0], options.scale[1]);
            transform.uvOffset          = Vec2(options.origin_offset[0], 1 - options.scale[1] - options.origin_offset[1]);
            transform.row0              = Vec4(transform.uvScale.x, 0, transform.uvOffset.x, 0);
            transform.row1              = Vec4(0, transform.uvScale.y, transform.uvOffset.y, 0);
        }
    }
    return result;
}
float Scalar(const ImagePixels& pixels, size_t pixel, char channel)
{
    const uint8_t* rgba  = pixels.bytes.data() + pixel * 4;
    float          value = (0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2]) / 255;
    if (channel == 'r')
    {
        value = rgba[0] / 255.0f;
    }
    if (channel == 'g')
    {
        value = rgba[1] / 255.0f;
    }
    if (channel == 'b')
    {
        value = rgba[2] / 255.0f;
    }
    if (channel == 'm')
    {
        value = rgba[3] / 255.0f;
    }
    return value;
}
ObjMap ObjTextures::Pack(const ObjMap& first, const ObjMap& second, bool alpha, float exponent, const std::string& name)
{
    ObjMap              result = first.binding.texture.IsValid() ? first : second;
    const TextureAsset* a = first.binding.texture.IsValid() ? &m_state.scene.textures[first.binding.texture.value] : nullptr;
    const TextureAsset* b = second.binding.texture.IsValid() ? &m_state.scene.textures[second.binding.texture.value] : nullptr;
    if ((a != nullptr || b != nullptr) && !m_state.errors.Failed())
    {
        const TextureAsset selected   = a != nullptr ? *a : *b;
        const ImagePixels* pa         = a != nullptr ? m_state.scene.images[a->image.value].pixels.get() : nullptr;
        const ImagePixels* pb         = b != nullptr ? m_state.scene.images[b->image.value].pixels.get() : nullptr;
        const ImagePixels& dimensions = pa != nullptr ? *pa : *pb;
        bool               compatible = true;
        if (pa != nullptr && pb != nullptr)
        {
            const SamplerAsset& sa = m_state.scene.samplers[a->sampler.value];
            const SamplerAsset& sb = m_state.scene.samplers[b->sampler.value];
            compatible             = pa->width == pb->width && pa->height == pb->height
                                  && first.binding.transform == second.binding.transform && sa.wrapS == sb.wrapS && sa.wrapT == sb.wrapT;
        }
        if (!compatible)
        {
            m_state.errors.Fail("MTL maps need identical dimensions/transforms/wrap for channel packing: " + name,
                                StatusCode::UnsupportedFeature);
        }
        else if (m_state.errors.ReserveImageBytes(dimensions.bytes.size()))
        {
            std::shared_ptr<ImagePixels> pixels(new ImagePixels);
            pixels->width  = dimensions.width;
            pixels->height = dimensions.height;
            pixels->bytes.resize(dimensions.bytes.size(), 255);
            for (size_t index = 0; !m_state.errors.Failed() && index < pixels->bytes.size() / 4; ++index)
            {
                if (alpha)
                {
                    if (pa != nullptr)
                    {
                        std::copy_n(pa->bytes.data() + index * 4, 4, pixels->bytes.data() + index * 4);
                    }
                    const float opacity          = pb != nullptr ? Scalar(*pb, index, second.channel) : 1;
                    pixels->bytes[index * 4 + 3] = static_cast<uint8_t>(std::lround(pixels->bytes[index * 4 + 3] * opacity));
                }
                else
                {
                    float roughness = pa != nullptr ? Scalar(*pa, index, first.channel) : 1;
                    if (exponent >= 0)
                    {
                        roughness = std::sqrt(2 / (exponent * roughness + 2));
                    }
                    pixels->bytes[index * 4 + 1] = static_cast<uint8_t>(std::lround(std::clamp(roughness, 0.0f, 1.0f) * 255));
                    const float metal            = pb != nullptr ? Scalar(*pb, index, second.channel) : 1;
                    pixels->bytes[index * 4 + 2] = static_cast<uint8_t>(std::lround(std::clamp(metal, 0.0f, 1.0f) * 255));
                }
            }
            TextureAsset texture = selected;
            texture.name         = name;
            texture.image.value  = static_cast<uint32_t>(m_state.scene.images.size());
            texture.format       = alpha ? (a != nullptr ? a->format : PixelFormat::RGBA8Srgb) : PixelFormat::RGBA8Unorm;
            m_state.scene.images.push_back({name, pixels});
            result.binding.texture.value = static_cast<uint32_t>(m_state.scene.textures.size());
            m_state.scene.textures.push_back(std::move(texture));
        }
    }
    return result;
}
} // namespace

bool ObjMaterialReader::operator()(const std::string&                materialFile,
                                   std::vector<tinyobj::material_t>* materials,
                                   std::map<std::string, int>*       materialMap,
                                   std::string*                      warning,
                                   std::string*                      error)
{
    const std::filesystem::path path = ObjPath(m_state, m_state.request.path).parent_path() / ObjPath(m_state, materialFile);
    std::string                 bytes;
    bool                        valid = LocalDependency(m_state, path) && m_state.ReadFile(path, bytes);
    if (valid && m_state.Charge(bytes.size(), 8))
    {
        const size_t                             previous = materials->size();
        std::map<std::string, ObjMaterialFields> fields;
        const std::string                        normalized = PrepareMtl(m_state, bytes, fields);
        std::istringstream                       stream(normalized);
        tinyobj::LoadMtl(materialMap, materials, &stream, warning, error);
        for (size_t index = previous; !m_state.errors.Failed() && index < materials->size(); ++index)
        {
            const tinyobj::material_t& material = (*materials)[index];
            m_state.Charge(1, sizeof(MaterialAsset) + sizeof(tinyobj::material_t) + 256);
            for (size_t earlier = 0; earlier < index; ++earlier)
            {
                if ((*materials)[earlier].name == material.name)
                {
                    m_state.errors.Fail("Duplicate MTL material name: " + material.name);
                }
            }
            m_state.materialDirectories.push_back(path.parent_path());
            m_state.materialFields.push_back(fields[material.name]);
        }
        valid = !m_state.errors.Failed();
        if (valid)
        {
            m_state.CompleteDependency(path);
        }
    }
    return valid;
}

void ReadObjMaterials(ObjImportState& state, const std::vector<tinyobj::material_t>& materials)
{
    MaterialAsset fallback;
    fallback.name           = "OBJ default";
    fallback.metallicFactor = 0;
    if (state.Charge(1, sizeof(MaterialAsset)))
    {
        state.scene.materials.push_back(fallback);
    }
    ObjTextures textures(state);
    for (size_t index = 0; !state.errors.Failed() && index < materials.size(); ++index)
    {
        const tinyobj::material_t&   source    = materials[index];
        const std::filesystem::path& directory = state.materialDirectories[index];
        const ObjMaterialFields&     fields    = state.materialFields[index];
        MaterialAsset                material;
        material.name = source.name;
        material.baseColorFactor =
            Vec4(!fields.diffuse && !source.diffuse_texname.empty() ? Vec3(1) : MakeVec3(source.diffuse), source.dissolve);
        material.emissiveFactor =
            Vec4(!fields.emission && !source.emissive_texname.empty() ? Vec3(1) : MakeVec3(source.emission), 0);
        material.metallicFactor  = !source.metallic_texname.empty() && !fields.metalness ? 1 : source.metallic;
        material.roughnessFactor = fields.roughness ? source.roughness : std::sqrt(2 / (std::max(0.0f, source.shininess) + 2));
        material.features.specularColor =
            !fields.specular && !source.specular_texname.empty() ? Vec3(1) : MakeVec3(source.specular);
        material.features.ior = source.ior >= 1 ? source.ior : 1.5f;
        material.unlit        = fields.illumination && source.illum == 0;
        state.Warn(material.name, "Legacy MTL shading approximated by PBR; Ns maps to sqrt(2/(Ns+2)), Ks to specular color");
        ObjMap       base    = textures.Read(source.diffuse_texname, source.diffuse_texopt, directory, true);
        const ObjMap opacity = textures.Read(source.alpha_texname, source.alpha_texopt, directory, false);
        if (opacity.binding.texture.IsValid())
        {
            base = textures.Pack(base, opacity, true, -1, material.name + " base/opacity");
        }
        material.m_pBaseColorTexture  = base.binding.texture;
        material.textureTransforms[0] = base.binding.transform;
        const ObjMap emission         = textures.Read(source.emissive_texname, source.emissive_texopt, directory, true);
        material.m_pEmissiveTexture   = emission.binding.texture;
        material.textureTransforms[4] = emission.binding.transform;
        material.features.specularColorTexture =
            textures.Read(source.specular_texname, source.specular_texopt, directory, true).binding;
        const bool   phongMap = source.roughness_texname.empty() && !source.specular_highlight_texname.empty();
        const ObjMap roughness =
            textures.Read(phongMap ? source.specular_highlight_texname : source.roughness_texname,
                          phongMap ? source.specular_highlight_texopt : source.roughness_texopt, directory, false);
        const ObjMap metal = textures.Read(source.metallic_texname, source.metallic_texopt, directory, false);
        if (roughness.binding.texture.IsValid() || metal.binding.texture.IsValid())
        {
            const ObjMap packed                  = textures.Pack(roughness, metal, false, phongMap ? source.shininess : -1,
                                                                 material.name + " roughness/metalness");
            material.m_pMetallicRoughnessTexture = packed.binding.texture;
            material.textureTransforms[1]        = packed.binding.transform;
            if (phongMap || (!source.roughness_texname.empty() && !fields.roughness))
            {
                material.roughnessFactor = 1;
            }
        }
        const ObjMap normal           = textures.Read(source.normal_texname, source.normal_texopt, directory, false, true);
        material.m_pNormalTexture     = normal.binding.texture;
        material.normalScale          = source.normal_texopt.bump_multiplier;
        material.textureTransforms[2] = normal.binding.transform;
        if (source.dissolve < 1 || opacity.binding.texture.IsValid())
        {
            material.alphaMode = AlphaMode::Blend;
        }
        if (!source.bump_texname.empty())
        {
            state.Warn(material.name, "MTL bump is a height map, not a tangent-space normal map; ignored");
        }
        if (source.ambient[0] != 0 || source.ambient[1] != 0 || source.ambient[2] != 0 || source.illum > 2)
        {
            state.Warn(material.name,
                       "MTL ambient lighting and reflection/refraction illumination models are not reproduced by PBR");
        }
        if (!source.ambient_texname.empty() || !source.displacement_texname.empty() || !source.reflection_texname.empty()
            || !source.sheen_texname.empty())
        {
            state.Warn(material.name, "Unsupported MTL ambient/displacement/reflection/sheen maps were ignored");
        }
        if (source.clearcoat_thickness != 0 || source.sheen != 0 || source.anisotropy != 0 || source.transmittance[0] != 0
            || source.transmittance[1] != 0 || source.transmittance[2] != 0)
        {
            state.Warn(material.name, "Unsupported MTL transmission/coat/sheen/anisotropy factors were ignored");
        }
        for (const std::pair<const std::string, std::string>& parameter : source.unknown_parameter)
        {
            state.Warn(material.name, "Unsupported MTL parameter: " + parameter.first);
        }
        state.scene.materials.push_back(std::move(material));
    }
}
} // namespace zenasset
