#include <future>
#include "Diagnostics/StageMeasurements.h"
#include <cmath>
#include <algorithm>
#include <cctype>
#include <climits>
#include <mutex>
#include <fstream>
#include <map>
#include <tuple>
#include <charconv>
#include <type_traits>
#include <meshoptimizer.h>
#include <draco/compression/decode.h>
#include <simdjson.h>
#include <stb_image.h>
#include "Importers/GltfAssetReader.h"
#include "Importers/ImageDecoder.h"
#include "ZenAsset/Processor/AnimationTargets.h"
#include "Processing/SurfaceGeometry.h"
#include "Importers/AssetHash.h"
#include "Importers/AssetPaths.h"
#include "Importers/GltfElementTraits.h"
#include "Format/MathOps.h"
#include "Processor/WorkerPool.h"

namespace zenasset
{
static fastgltf::span<const std::byte> GetBufferBytes(const fastgltf::Asset& asset,
                                                      size_t                 bufferIndex,
                                                      size_t                 offset,
                                                      size_t                 length,
                                                      ImportErrors&          errors)
{
    fastgltf::span<const std::byte> bytes;

    if (bufferIndex < asset.buffers.size())
    {
        const fastgltf::DataSource& source = asset.buffers[bufferIndex].data;

        if (const fastgltf::sources::Array* array = std::get_if<fastgltf::sources::Array>(&source))
        {
            bytes = fastgltf::span<const std::byte>(array->bytes.data(), array->bytes.size());
        }
        else if (const fastgltf::sources::Vector* vector = std::get_if<fastgltf::sources::Vector>(&source))
        {
            bytes = fastgltf::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
        }
        else if (const fastgltf::sources::ByteView* view = std::get_if<fastgltf::sources::ByteView>(&source))
        {
            bytes = view->bytes;
        }
    }

    fastgltf::span<const std::byte> result;

    if (offset > bytes.size() || length > bytes.size() - offset)
    {
        errors.Fail("glTF buffer range is outside its loaded data");
    }
    else
    {
        result = bytes.subspan(offset, length);
    }

    return result;
}

static size_t AddDecodedBuffer(fastgltf::Asset& asset, std::vector<std::byte> bytes)
{
    const size_t index = asset.buffers.size();

    fastgltf::Buffer buffer;

    buffer.byteLength = bytes.size();

    buffer.data       = fastgltf::sources::Vector{std::move(bytes)};

    asset.buffers.push_back(std::move(buffer));

    return index;
}

static void DecodeMeshoptBuffer(fastgltf::Asset& asset, fastgltf::BufferView& view, ImportErrors& errors);

static void DecodeMeshoptBuffers(fastgltf::Asset& asset, ImportErrors& errors)
{
    // Decoding appends buffers, but buffer views stay in place.
    for (size_t index = 0; index < asset.bufferViews.size() && !errors.Failed(); ++index)
    {
        if (asset.bufferViews[index].meshoptCompression)
        {
            DecodeMeshoptBuffer(asset, asset.bufferViews[index], errors);
        }
    }
}

static void DecodeMeshoptBuffer(fastgltf::Asset& asset, fastgltf::BufferView& view, ImportErrors& errors)
{
    const fastgltf::CompressedBufferView& compression = *view.meshoptCompression;

    const fastgltf::span<const std::byte> source =
        GetBufferBytes(asset, compression.bufferIndex, compression.byteOffset, compression.byteLength, errors);

    const bool sizeValid   = compression.byteStride != 0 && compression.count <= SIZE_MAX / compression.byteStride;

    const bool vertexMode  = compression.mode == fastgltf::MeshoptCompressionMode::Attributes;

    const bool strideValid = vertexMode ? compression.byteStride <= 256 && compression.byteStride % 4 == 0
                                        : compression.byteStride == 2 || compression.byteStride == 4;

    const bool filterValid =
        compression.filter == fastgltf::MeshoptCompressionFilter::None
        || (vertexMode
            && (compression.filter == fastgltf::MeshoptCompressionFilter::Exponential
                || ((compression.filter == fastgltf::MeshoptCompressionFilter::Octahedral
                     || compression.filter == fastgltf::MeshoptCompressionFilter::Color)
                    && (compression.byteStride == 4 || compression.byteStride == 8))
                || (compression.filter == fastgltf::MeshoptCompressionFilter::Quaternion && compression.byteStride == 8)));

    if (!sizeValid)
    {
        errors.Fail("Invalid meshopt decoded buffer size");
    }
    else if (!strideValid || !filterValid
             || (compression.mode == fastgltf::MeshoptCompressionMode::Triangles && compression.count % 3 != 0))
    {
        errors.Fail("Invalid meshopt compression mode, stride or filter");
    }

    std::vector<std::byte> decoded;
    if (!errors.Failed() && ResizeGeometryVector(decoded, compression.count * compression.byteStride, errors))
    {
        const unsigned char* input = reinterpret_cast<const unsigned char*>(source.data());

        int status                 = -1;

        switch (compression.mode)
        {
            case fastgltf::MeshoptCompressionMode::Attributes:
                status =
                    meshopt_decodeVertexBuffer(decoded.data(), compression.count, compression.byteStride, input, source.size());

                break;

            case fastgltf::MeshoptCompressionMode::Triangles:
                status =
                    meshopt_decodeIndexBuffer(decoded.data(), compression.count, compression.byteStride, input, source.size());

                break;

            case fastgltf::MeshoptCompressionMode::Indices:
                status = meshopt_decodeIndexSequence(decoded.data(), compression.count, compression.byteStride, input,
                                                     source.size());

                break;
        }

        if (status != 0)
        {
            errors.Fail(("Failed to decode meshopt buffer (" + std::to_string(status) + ")"));
        }
        else
        {
            switch (compression.filter)
            {
                case fastgltf::MeshoptCompressionFilter::Octahedral:
                    meshopt_decodeFilterOct(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Quaternion:
                    meshopt_decodeFilterQuat(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Exponential:
                    meshopt_decodeFilterExp(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Color:
                    meshopt_decodeFilterColor(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::None: break;
            }

            view.bufferIndex = AddDecodedBuffer(asset, std::move(decoded));

            view.byteOffset  = 0;

            view.byteLength  = compression.count * compression.byteStride;

            view.meshoptCompression.reset();
        }
    }
}

// A value that cannot be converted records an error; the caller discards the bytes.
template <typename T> static void DecodeDracoAttributeValues(const draco::Mesh&           mesh,
                                                             const draco::PointAttribute& attribute,
                                                             std::vector<std::byte>&      bytes,
                                                             ImportErrors&                errors)
{
    const size_t components = attribute.num_components();

    ResizeGeometryVector(bytes, size_t(mesh.num_points()) * components * sizeof(T), errors);

    for (uint32_t index = 0; index < mesh.num_points() && !errors.Failed(); ++index)
    {
        T values[16]{};

        if (components > 16
            || !attribute.ConvertValue(attribute.mapped_index(draco::PointIndex(index)), static_cast<int8_t>(components),
                                       values))
        {
            errors.Fail("Failed to convert Draco vertex attribute");
        }
        else
        {
            std::memcpy(bytes.data() + size_t(index) * components * sizeof(T), values, components * sizeof(T));
        }
    }
}

static void SetDecodedAccessor(fastgltf::Asset& asset, fastgltf::Accessor& accessor, std::vector<std::byte> bytes)
{
    fastgltf::BufferView view;

    view.byteLength          = bytes.size();

    view.bufferIndex         = AddDecodedBuffer(asset, std::move(bytes));

    accessor.bufferViewIndex = asset.bufferViews.size();

    accessor.byteOffset      = 0;

    accessor.sparse.reset();

    asset.bufferViews.push_back(std::move(view));
}

static void DecodeDracoPrimitive(fastgltf::Asset& asset, fastgltf::Primitive& primitive, ImportErrors& errors);

static void DecodeDracoAttribute(fastgltf::Asset&           asset,
                                 const fastgltf::Primitive& primitive,
                                 const fastgltf::Attribute& encoded,
                                 const draco::Mesh&         mesh,
                                 ImportErrors&              errors);

static void DecodeDracoMeshes(fastgltf::Asset& asset, ImportErrors& errors)
{
    // Decoding appends buffers, buffer views and accessor data, but meshes stay in place.
    for (fastgltf::Mesh& sourceMesh : asset.meshes)
    {
        for (fastgltf::Primitive& primitive : sourceMesh.primitives)
        {
            if (primitive.dracoCompression && !errors.Failed())
            {
                DecodeDracoPrimitive(asset, primitive, errors);
            }
        }
    }
}

static void DecodeDracoPrimitive(fastgltf::Asset& asset, fastgltf::Primitive& primitive, ImportErrors& errors)
{
    const fastgltf::DracoCompressedPrimitive& compression = *primitive.dracoCompression;

    if (compression.bufferView >= asset.bufferViews.size())
    {
        errors.Fail("Draco references an invalid buffer view");
    }
    else if (primitive.indicesAccessor && *primitive.indicesAccessor >= asset.accessors.size())
    {
        errors.Fail("Draco references an invalid indices accessor");
    }

    for (const fastgltf::Attribute& encoded : compression.attributes)
    {
        const fastgltf::Attribute* destination = primitive.findAttribute(encoded.name);

        if (destination == primitive.attributes.end() || destination->accessorIndex >= asset.accessors.size()
            || encoded.accessorIndex > UINT32_MAX)
        {
            errors.Fail("Invalid Draco attribute mapping");
        }
    }

    std::unique_ptr<draco::Mesh> mesh;

    if (!errors.Failed())
    {
        const fastgltf::BufferView& view = asset.bufferViews[compression.bufferView];

        const fastgltf::span<const std::byte> bytes =
            GetBufferBytes(asset, view.bufferIndex, view.byteOffset, view.byteLength, errors);

        if (!errors.Failed())
        {
            draco::DecoderBuffer buffer;

            buffer.Init(reinterpret_cast<const char*>(bytes.data()), bytes.size());

            draco::Decoder decoder;

            draco::StatusOr<std::unique_ptr<draco::Mesh>> result = decoder.DecodeMeshFromBuffer(&buffer);

            if (result.ok())
            {
                mesh = std::move(result).value();
            }
            else
            {
                errors.Fail((std::string("Failed to decode Draco mesh: ") + std::string(result.status().error_msg())));
            }
        }
    }

    for (const fastgltf::Attribute& encoded : compression.attributes)
    {
        if (mesh != nullptr && !errors.Failed())
        {
            DecodeDracoAttribute(asset, primitive, encoded, *mesh, errors);
        }
    }

    if (mesh != nullptr && !errors.Failed())
    {
        if (primitive.indicesAccessor)
        {
            fastgltf::Accessor& accessor = asset.accessors[*primitive.indicesAccessor];

            accessor.count               = size_t(mesh->num_faces()) * 3;

            accessor.componentType       = fastgltf::ComponentType::UnsignedInt;

            std::vector<std::byte> indices;
            ResizeGeometryVector(indices, accessor.count * sizeof(uint32_t), errors);

            for (uint32_t face = 0; !errors.Failed() && face < mesh->num_faces(); ++face)
            {
                const draco::Mesh::Face& triangle = mesh->face(draco::FaceIndex(face));

                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    const uint32_t index = triangle[corner].value();

                    std::memcpy(indices.data() + (size_t(face) * 3 + corner) * sizeof(uint32_t), &index, sizeof(index));
                }
            }

            if (!errors.Failed())
            {
                SetDecodedAccessor(asset, accessor, std::move(indices));
            }
        }

        primitive.type = fastgltf::PrimitiveType::Triangles;

        primitive.dracoCompression.reset();
    }
}

static void DecodeDracoAttribute(fastgltf::Asset&           asset,
                                 const fastgltf::Primitive& primitive,
                                 const fastgltf::Attribute& encoded,
                                 const draco::Mesh&         mesh,
                                 ImportErrors&              errors)
{
    const fastgltf::Attribute* destination = primitive.findAttribute(encoded.name);

    const draco::PointAttribute* attribute = mesh.GetAttributeByUniqueId(static_cast<uint32_t>(encoded.accessorIndex));

    if (destination == primitive.attributes.end() || attribute == nullptr
        || destination->accessorIndex >= asset.accessors.size())
    {
        errors.Fail("Invalid Draco attribute mapping");
    }
    else if (fastgltf::getNumComponents(asset.accessors[destination->accessorIndex].type) != attribute->num_components())
    {
        errors.Fail("Draco attribute has an incompatible component count");
    }
    else
    {
        fastgltf::Accessor& accessor = asset.accessors[destination->accessorIndex];

        std::vector<std::byte> decoded;

        switch (accessor.componentType)
        {
            case fastgltf::ComponentType::Byte: DecodeDracoAttributeValues<int8_t>(mesh, *attribute, decoded, errors); break;
            case fastgltf::ComponentType::UnsignedByte:
                DecodeDracoAttributeValues<uint8_t>(mesh, *attribute, decoded, errors);
                break;
            case fastgltf::ComponentType::Short: DecodeDracoAttributeValues<int16_t>(mesh, *attribute, decoded, errors); break;
            case fastgltf::ComponentType::UnsignedShort:
                DecodeDracoAttributeValues<uint16_t>(mesh, *attribute, decoded, errors);
                break;
            case fastgltf::ComponentType::Int: DecodeDracoAttributeValues<int32_t>(mesh, *attribute, decoded, errors); break;
            case fastgltf::ComponentType::UnsignedInt:
                DecodeDracoAttributeValues<uint32_t>(mesh, *attribute, decoded, errors);
                break;
            case fastgltf::ComponentType::Float: DecodeDracoAttributeValues<float>(mesh, *attribute, decoded, errors); break;
            case fastgltf::ComponentType::Double: DecodeDracoAttributeValues<double>(mesh, *attribute, decoded, errors); break;
            default: errors.Fail("Invalid Draco attribute component type"); break;
        }

        if (!errors.Failed())
        {
            accessor.count = mesh.num_points();

            SetDecodedAccessor(asset, accessor, std::move(decoded));
        }
    }
}

static TextureFilter FromFastGltfFilter(fastgltf::Optional<fastgltf::Filter> filter)
{
    TextureFilter result = TextureFilter::Linear;

    switch (filter.value_or(fastgltf::Filter::Linear))
    {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::NearestMipMapLinear:
        {
            result = TextureFilter::Nearest;

            break;
        }
        case fastgltf::Filter::Linear:
        case fastgltf::Filter::LinearMipMapNearest:
        case fastgltf::Filter::LinearMipMapLinear:
        {
            result = TextureFilter::Linear;

            break;
        }
        default: break;
    }

    return result;
}

static SamplerAddressMode FromFastGltfWrap(fastgltf::Wrap wrap)
{
    SamplerAddressMode result = SamplerAddressMode::Repeat;

    switch (wrap)
    {
        case fastgltf::Wrap::Repeat:
        {
            result = SamplerAddressMode::Repeat;

            break;
        }
        case fastgltf::Wrap::ClampToEdge:
        {
            result = SamplerAddressMode::ClampToEdge;

            break;
        }
        case fastgltf::Wrap::MirroredRepeat:
        {
            result = SamplerAddressMode::MirroredRepeat;

            break;
        }
        default: break;
    }

    return result;
}

static fastgltf::Error ValidateSceneAsset(fastgltf::Asset& asset)
{
    bool referencesValid = !asset.defaultScene || *asset.defaultScene < asset.scenes.size();

    for (const fastgltf::Node& node : asset.nodes)
    {
        referencesValid &= !node.lightIndex || *node.lightIndex < asset.lights.size();

        for (size_t child : node.children)
        {
            referencesValid &= child < asset.nodes.size();
        }
    }

    if (referencesValid)
    {
        std::vector<uint32_t> parents(asset.nodes.size(), 0);

        for (const fastgltf::Node& node : asset.nodes)
        {
            for (size_t child : node.children)
            {
                referencesValid &= ++parents[child] == 1;
            }
        }

        std::vector<size_t> pending;

        for (size_t index = 0; index < parents.size(); ++index)
        {
            if (parents[index] == 0)
            {
                pending.push_back(index);
            }
        }

        for (size_t head = 0; head < pending.size(); ++head)
        {
            for (size_t child : asset.nodes[pending[head]].children)
            {
                if (--parents[child] == 0)
                {
                    pending.push_back(child);
                }
            }
        }

        referencesValid &= pending.size() == asset.nodes.size();
    }

    for (const fastgltf::Animation& animation : asset.animations)
    {
        for (const fastgltf::AnimationChannel& channel : animation.channels)
        {
            referencesValid &= channel.samplerIndex < animation.samplers.size()
                            && (!channel.nodeIndex || *channel.nodeIndex < asset.nodes.size());
        }

        for (const fastgltf::AnimationSampler& sampler : animation.samplers)
        {
            referencesValid &=
                sampler.inputAccessor < asset.accessors.size() && sampler.outputAccessor < asset.accessors.size();
        }
    }

    for (const fastgltf::Skin& skin : asset.skins)
    {
        for (size_t joint : skin.joints)
        {
            referencesValid &= joint < asset.nodes.size();
        }

        if (skin.inverseBindMatrices && *skin.inverseBindMatrices < asset.accessors.size())
        {
            const fastgltf::Accessor& accessor  = asset.accessors[*skin.inverseBindMatrices];

            referencesValid                    &= accessor.type == fastgltf::AccessorType::Mat4
                                               && accessor.componentType == fastgltf::ComponentType::Float && accessor.count >= skin.joints.size();
        }
    }

    for (const fastgltf::Mesh& mesh : asset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& target : primitive.targets)
            {
                for (const fastgltf::Attribute& attribute : target)
                {
                    referencesValid &= attribute.accessorIndex < asset.accessors.size();

                    if (attribute.accessorIndex < asset.accessors.size())
                    {
                        const fastgltf::AccessorType type = asset.accessors[attribute.accessorIndex].type;

                        referencesValid &=
                            attribute.name.starts_with("TEXCOORD_") ? type == fastgltf::AccessorType::Vec2
                            : attribute.name.starts_with("COLOR_")
                                ? type == fastgltf::AccessorType::Vec3 || type == fastgltf::AccessorType::Vec4
                                : (attribute.name != "POSITION" && attribute.name != "NORMAL" && attribute.name != "TANGENT")
                                      || type == fastgltf::AccessorType::Vec3;

                        const fastgltf::Attribute* position = primitive.findAttribute("POSITION");

                        if (position != primitive.attributes.end() && position->accessorIndex < asset.accessors.size())
                        {
                            referencesValid &= asset.accessors[attribute.accessorIndex].count
                                            == asset.accessors[position->accessorIndex].count;
                        }
                    }
                }
            }
        }
    }

    // fastgltf 0.9.1 compares float cone angles with a long-double pi/2. A valid
    // 90-degree angle rounds slightly above that limit. Validate the adjacent float
    // and then restore the exact authored value for the renderer.
    std::vector<std::pair<size_t, float>> coneLimits;

    for (size_t index = 0; index < asset.lights.size(); ++index)
    {
        fastgltf::Light& light = asset.lights[index];

        if (light.outerConeAngle && *light.outerConeAngle == HalfPi)
        {
            coneLimits.push_back({index, *light.outerConeAngle});

            light.outerConeAngle = std::nextafter(HalfPi, 0.0f);
        }
    }

    const fastgltf::Error result = referencesValid ? fastgltf::validate(asset) : fastgltf::Error::InvalidGltf;

    for (const std::pair<size_t, float>& cone : coneLimits)
    {
        asset.lights[cone.first].outerConeAngle = cone.second;
    }

    return result;
}

GltfAssetReader::GltfAssetReader(const BackendContext& context) :
    m_workers(context.processing), m_imageBudget(context.images), m_geometryBudget(context.geometry)
{
    if (m_workers == nullptr)
    {
        m_ownedWorkers = std::make_unique<WorkerPool>(std::max(1u, std::thread::hardware_concurrency()));
        m_workers      = m_ownedWorkers.get();
    }
    assert(m_workers->GetSize() > 0);

    // TODO(glTF KHR_gaussian_splatting): import splat attributes and add a Gaussian renderer.
    // TODO(glTF KHR_interactivity): execute behavior graphs and connect their scene mutations.
    // TODO(glTF EXT_lights_ies): decode IES profiles and apply angular light distributions.
    // TODO(glTF EXT_mesh_manifold): retain manifold topology and implement its mesh semantics.
    // TODO(glTF vendor systems): add MSFT_lod, MSFT_texture_dds, MSFT packing and remaining
    // vendor-specific runtime handlers. Preserve optional metadata; reject required use by name.
    // TODO(glTF KHR_accessor_float64): add double-precision animation interpolation and
    // high-precision geometry conversion before claiming support for required float64 assets.
    static constexpr fastgltf::Extensions supportedExtensions{
        fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_lights_punctual
        | fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_texture_transform
        | fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_materials_specular
        | fastgltf::Extensions::KHR_materials_ior | fastgltf::Extensions::KHR_materials_clearcoat
        | fastgltf::Extensions::KHR_materials_sheen | fastgltf::Extensions::KHR_materials_transmission
        | fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_materials_iridescence
        | fastgltf::Extensions::KHR_materials_anisotropy | fastgltf::Extensions::KHR_materials_dispersion
        | fastgltf::Extensions::KHR_materials_diffuse_transmission | fastgltf::Extensions::KHR_materials_pbrSpecularGlossiness
        | fastgltf::Extensions::KHR_draco_mesh_compression | fastgltf::Extensions::EXT_meshopt_compression
        | fastgltf::Extensions::KHR_meshopt_compression | fastgltf::Extensions::KHR_texture_basisu
        | fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::EXT_mesh_gpu_instancing
        | fastgltf::Extensions::KHR_materials_variants};

    m_gltfParser = fastgltf::Parser(supportedExtensions);

    m_loadOptions =
        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages | fastgltf::Options::GenerateMeshIndices;
}

static void NormalizeWindowsFileUris(const simdjson::dom::element& document, std::string& normalized)
{
#if defined(_WIN32)
    for (const char* collection : {"buffers", "images"})
    {
        simdjson::dom::array entries;

        if (document[collection].get_array().get(entries) == simdjson::SUCCESS)
        {
            for (simdjson::dom::element entry : entries)
            {
                std::string_view uri;

                if (entry["uri"].get_string().get(uri) == simdjson::SUCCESS && uri.size() >= 11 && uri.starts_with("file:///")
                    && std::isalpha(static_cast<unsigned char>(uri[8])) && uri[9] == ':' && uri[10] == '/')
                {
                    if (normalized.empty())
                    {
                        normalized = simdjson::to_string(document);
                    }

                    const std::string original = simdjson::to_string(entry["uri"]);

                    // fastgltf's Windows URI adapter otherwise treats /D:/ as a path
                    // rooted on the current drive rather than the file URI's D: drive.
                    const std::string replacement = "\"file:" + original.substr(9);

                    size_t offset                 = normalized.find(original);

                    while (offset != std::string::npos)
                    {
                        normalized.replace(offset, original.size(), replacement);

                        offset = normalized.find(original, offset + replacement.size());
                    }
                }
            }
        }
    }
#endif
}

static std::string NormalizeIntegralJsonNumber(std::string_view number)
{
    std::string result(number);

    const size_t exponentPosition = number.find_first_of("eE");

    const size_t mantissaEnd      = exponentPosition == std::string_view::npos ? number.size() : exponentPosition;

    const size_t decimalPosition  = number.find('.');

    if (decimalPosition != std::string_view::npos || exponentPosition != std::string_view::npos)
    {
        const bool negative       = number.front() == '-';

        const size_t integerBegin = negative ? 1 : 0;

        const size_t integerEnd   = decimalPosition == std::string_view::npos ? mantissaEnd : decimalPosition;

        std::string digits;

        bool valid =
            integerEnd > integerBegin && integerEnd <= mantissaEnd
            && (integerEnd - integerBegin == 1 || number[integerBegin] != '0')
            && (decimalPosition == std::string_view::npos
                || (decimalPosition + 1 < mantissaEnd && number.find('.', decimalPosition + 1) == std::string_view::npos));

        for (size_t index = negative ? 1 : 0; index < mantissaEnd; ++index)
        {
            if (number[index] != '.')
            {
                valid  &= number[index] >= '0' && number[index] <= '9';

                digits += number[index];
            }
        }

        uint64_t exponent     = 0;

        bool negativeExponent = false;

        bool exponentOverflow = false;

        if (exponentPosition != std::string_view::npos)
        {
            size_t begin = exponentPosition + 1;

            if (begin < number.size() && (number[begin] == '-' || number[begin] == '+'))
            {
                negativeExponent = number[begin] == '-';

                ++begin;
            }

            const std::from_chars_result parsed =
                std::from_chars(number.data() + begin, number.data() + number.size(), exponent);

            exponentOverflow  = parsed.ec == std::errc::result_out_of_range;

            valid            &= begin < number.size() && (parsed.ec == std::errc{} || exponentOverflow)
                             && parsed.ptr == number.data() + number.size();
        }

        const size_t first = digits.find_first_not_of('0');

        if (valid && !digits.empty() && first == std::string::npos)
        {
            result = negative ? "-0" : "0";
        }
        else if (valid && !exponentOverflow && first != std::string::npos)
        {
            digits.erase(0, first);

            const size_t fraction = decimalPosition == std::string_view::npos ? 0 : mantissaEnd - decimalPosition - 1;

            size_t removed        = 0;

            if (negativeExponent)
            {
                valid &= exponent <= digits.size() && fraction <= digits.size() && exponent <= digits.size() - fraction;

                if (valid)
                {
                    removed = fraction + static_cast<size_t>(exponent);
                }
            }
            else if (exponent < fraction)
            {
                removed = fraction - static_cast<size_t>(exponent);
            }
            else
            {
                const uint64_t added  = exponent - fraction;

                valid                &= added <= 20 && digits.size() <= 20 - added;

                if (valid)
                {
                    digits.append(static_cast<size_t>(added), '0');
                }
            }

            valid &= removed <= digits.size();

            if (valid && removed != 0)
            {
                const size_t suffix  = digits.size() - removed;

                valid               &= digits.find_first_not_of('0', suffix) == std::string::npos;

                digits.resize(suffix);
            }

            uint64_t value                       = 0;

            const std::from_chars_result parsed  = std::from_chars(digits.data(), digits.data() + digits.size(), value);

            valid                               &= parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()
                                                && (!negative || value <= uint64_t(INT64_MAX) + 1);

            if (valid)
            {
                result = (negative ? "-" : "") + digits;
            }
        }
    }

    return result;
}

static std::string NormalizeIntegralJsonNumbers(const std::string& json)
{
    std::string result;

    result.reserve(json.size());

    bool quoted  = false;

    bool escaped = false;

    for (size_t index = 0; index < json.size(); ++index)
    {
        const char character = json[index];

        if (!quoted && (character == '-' || (character >= '0' && character <= '9')))
        {
            const size_t first = index;

            while (index + 1 < json.size()
                   && std::string_view("0123456789.eE+-").find(json[index + 1]) != std::string_view::npos)
            {
                ++index;
            }

            result += NormalizeIntegralJsonNumber(std::string_view(json).substr(first, index - first + 1));
        }
        else
        {
            result += character;

            if (quoted && escaped)
            {
                escaped = false;
            }
            else if (quoted && character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                quoted = !quoted;
            }
        }
    }

    return result;
}

// Unsupported required extensions or a bad binary layout record an error and return no input.
static std::vector<std::byte> PrepareExtensionParserInput(const std::string&        json,
                                                          const std::string&        sourceJson,
                                                          fastgltf::span<std::byte> fileBytes,
                                                          bool                      binary,
                                                          ImportErrors&             errors)
{
    std::vector<std::byte> result;

    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array required;

    std::string normalized = json != sourceJson ? json : std::string{};

    if (parser.parse(json).get(document) == simdjson::SUCCESS)
    {
        if (document["extensionsRequired"].get_array().get(required) == simdjson::SUCCESS)
        {
            static constexpr std::string_view parserExtensions[]{"KHR_materials_emissive_strength",
                                                                 "KHR_lights_punctual",
                                                                 "KHR_mesh_quantization",
                                                                 "KHR_texture_transform",
                                                                 "KHR_materials_unlit",
                                                                 "KHR_materials_specular",
                                                                 "KHR_materials_ior",
                                                                 "KHR_materials_clearcoat",
                                                                 "KHR_materials_sheen",
                                                                 "KHR_materials_transmission",
                                                                 "KHR_materials_volume",
                                                                 "KHR_materials_iridescence",
                                                                 "KHR_materials_anisotropy",
                                                                 "KHR_materials_dispersion",
                                                                 "KHR_materials_diffuse_transmission",
                                                                 "KHR_materials_pbrSpecularGlossiness",
                                                                 "KHR_draco_mesh_compression",
                                                                 "EXT_meshopt_compression",
                                                                 "KHR_meshopt_compression",
                                                                 "KHR_texture_basisu",
                                                                 "EXT_texture_webp",
                                                                 "EXT_mesh_gpu_instancing",
                                                                 "KHR_materials_variants"};

            std::string unsupported;

            bool engineExtension      = false;

            std::string requiredArray = "[";

            for (simdjson::dom::element entry : required)
            {
                std::string_view extension;

                if (entry.get_string().get(extension) == simdjson::SUCCESS)
                {
                    if (extension == "KHR_animation_pointer" || extension == "KHR_xmp_json_ld"
                        || extension == "EXT_lights_image_based" || extension == "KHR_materials_volume_scatter"
                        || extension == "KHR_materials_retroreflection" || extension == "KHR_node_visibility"
                        || extension == "KHR_node_selectability" || extension == "KHR_node_hoverability")
                    {
                        // The engine imports these payloads after fastgltf reads the ordinary asset.
                        engineExtension = true;
                    }
                    else
                    {
                        if (std::find(std::begin(parserExtensions), std::end(parserExtensions), extension)
                            == std::end(parserExtensions))
                        {
                            if (!unsupported.empty())
                            {
                                unsupported += ", ";
                            }

                            unsupported += extension;
                        }

                        if (requiredArray.size() > 1)
                        {
                            requiredArray += ',';
                        }

                        requiredArray += simdjson::to_string(entry);
                    }
                }
            }

            requiredArray += ']';

            if (!unsupported.empty())
            {
                errors.Fail((std::string("Unsupported required glTF extension(s): ") + std::string(unsupported)),
                            SceneImportStatus::UnsupportedFeature);
            }

            if (engineExtension && !errors.Failed())
            {
                // Replace only the root property; extras may contain the same key.
                normalized = "{";

                for (const simdjson::dom::key_value_pair field : document.get_object().value())
                {
                    if (normalized.size() > 1)
                    {
                        normalized += ',';
                    }

                    normalized += field.key == "extensionsRequired" ? "\"extensionsRequired\":" + requiredArray
                                                                    : simdjson::to_string(field);
                }

                normalized += '}';
            }
        }

        // Compose URI normalization with the structural extension rewrite.
        NormalizeWindowsFileUris(document, normalized);

        if (!normalized.empty() && !errors.Failed())
        {
            if (binary)
            {
                while (normalized.size() % 4 != 0)
                {
                    normalized += ' ';
                }

                const size_t tailOffset = 20 + sourceJson.size();

                if (tailOffset > fileBytes.size() || normalized.size() > UINT32_MAX - fileBytes.size())
                {
                    errors.Fail("Invalid glTF binary extension document size");
                }
                else
                {
                    const uint32_t jsonLength = static_cast<uint32_t>(normalized.size());

                    const uint32_t fileLength = static_cast<uint32_t>(20 + normalized.size() + fileBytes.size() - tailOffset);

                    result.resize(fileLength);

                    std::memcpy(result.data(), fileBytes.data(), 20);

                    std::memcpy(result.data() + 8, &fileLength, sizeof(fileLength));

                    std::memcpy(result.data() + 12, &jsonLength, sizeof(jsonLength));

                    std::memcpy(result.data() + 20, normalized.data(), normalized.size());

                    std::memcpy(result.data() + 20 + normalized.size(), fileBytes.data() + tailOffset,
                                fileBytes.size() - tailOffset);
                }
            }
            else
            {
                result.resize(normalized.size());

                std::memcpy(result.data(), normalized.data(), normalized.size());
            }
        }
    }

    return result;
}

static void LoadNodeProperties(const std::string& json, fastgltf::Asset& asset, ImportErrors& errors)
{
    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array nodes;

    if (parser.parse(json).get(document) == simdjson::SUCCESS && document["nodes"].get_array().get(nodes) == simdjson::SUCCESS)
    {
        size_t nodeIndex = 0;

        for (simdjson::dom::element node : nodes)
        {
            const char* paths[]{"/extensions/KHR_node_visibility/visible", "/extensions/KHR_node_selectability/selectable",
                                "/extensions/KHR_node_hoverability/hoverable"};

            fastgltf::Node& destination = asset.nodes[nodeIndex++];

            bool* flags[]{&destination.visible, &destination.selectable, &destination.hoverable};

            for (uint32_t index = 0; index < 3; ++index)
            {
                bool value                       = true;

                const simdjson::error_code error = node.at_pointer(paths[index]).get_bool().get(value);

                if (error != simdjson::SUCCESS && error != simdjson::NO_SUCH_FIELD)
                {
                    errors.Fail("Invalid glTF node interaction flag");
                }
                else
                {
                    *flags[index] = value;
                }
            }
        }
    }
}

void GltfFileProgress::CompleteFile(const std::string& path) const
{
    if (control != nullptr && !path.empty())
    {
        std::error_code error;
        const bool rootFile = std::filesystem::equivalent(std::filesystem::u8path(path), std::filesystem::u8path(root), error);
        if (!error && !rootFile)
        {
            control->CompleteFile(path);
        }
    }
}

void GltfFileProgress::CompleteImage(uint32_t index) const
{
    if (index < imageFiles.size())
    {
        CompleteFile(imageFiles[index]);
    }
}

std::unique_ptr<Scene> GltfAssetReader::LoadAsset(const std::string&         path,
                                                  const SceneImportSettings& settings,
                                                  SceneImportControl*        control)
{
    m_errors.Reset(control, m_imageBudget, m_geometryBudget);
    m_fileProgress                        = {control, path, {}};
    m_settings                            = settings;
    std::unique_ptr<Scene> candidate      = std::make_unique<Scene>();
    candidate->sourcePath                 = path;
    const std::filesystem::path filePath  = std::filesystem::u8path(path);
    std::string                 extension = filePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".gltf" && extension != ".glb")
    {
        m_errors.Fail("Unsupported scene format; expected .gltf or .glb");
    }
    else if (!m_errors.Failed())
    {
        m_name = PathToUtf8(filePath.stem());
        ImportFile(filePath, extension == ".glb", candidate.get());
    }
    if (control != nullptr && control->IsCancelled())
    {
        m_errors.Fail("Scene import cancelled");
    }
    m_errors.DetachControl();
    if (m_errors.Failed())
    {
        candidate.reset();
    }
    return candidate;
}

// Called before fastgltf reads external files (source == nullptr) to stamp them,
// then after the read to check that each file fastgltf consumed is the stamped
// revision. Stamping first lets cache publication reject edits made after it.
static void RecordGltfDependencies(const std::string&           json,
                                   const fastgltf::Asset*       source,
                                   const std::filesystem::path& filePath,
                                   Scene&                       asset,
                                   ImportErrors&                errors,
                                   GltfFileProgress&            progress)
{
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    const bool             parsed = parser.parse(json).get(document) == simdjson::SUCCESS;
    // Malformed JSON is left to fastgltf's error before the read.
    if (!parsed && source != nullptr)
    {
        errors.Fail("Could not inspect glTF dependencies");
    }
    for (const char* collection : {"buffers", "images"})
    {
        simdjson::dom::array entries;
        if (parsed && !errors.Failed() && document[collection].get_array().get(entries) == simdjson::SUCCESS)
        {
            size_t index = 0;
            for (simdjson::dom::element entry : entries)
            {
                std::string_view text;
                if (entry["uri"].get_string().get(text) == simdjson::SUCCESS)
                {
                    const fastgltf::URIView uri(text);
                    if (uri.isLocalPath())
                    {
                        const fastgltf::URI decoded(uri.path());
                        std::string_view    path = decoded.path();
#if defined(_WIN32)
                        if (path.size() >= 4 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1]))
                            && path[2] == ':' && path[3] == '/')
                        {
                            path.remove_prefix(1);
                        }
#endif
                        const std::string resolved = AbsoluteUtf8Path(filePath.parent_path() / std::filesystem::u8path(path));
                        if (source == nullptr && collection[0] == 'i')
                        {
                            progress.imageFiles.resize(index + 1);
                            progress.imageFiles[index] = resolved;
                        }
                        const SceneDependency* recorded = nullptr;
                        for (const SceneDependency& dependency : asset.dependencies)
                        {
                            recorded = dependency.path == resolved ? &dependency : recorded;
                        }
                        AssetFileStamp stamp;
                        if (source == nullptr)
                        {
                            // A missing file is left to fastgltf, which reports it.
                            if (!resolved.empty() && recorded == nullptr && StampAssetFile(resolved, stamp))
                            {
                                asset.dependencies.push_back({resolved, true, stamp.size, stamp.modified, 0});
                                if (progress.control != nullptr)
                                {
                                    progress.control->RegisterFile(resolved);
                                }
                            }
                        }
                        else
                        {
                            const fastgltf::DataSource* data =
                                collection[0] == 'b' ? (index < source->buffers.size() ? &source->buffers[index].data : nullptr)
                                                     : (index < source->images.size() ? &source->images[index].data : nullptr);
                            // LoadExternalBuffers/Images stores each complete file in an Array.
                            const fastgltf::sources::Array* bytes =
                                data != nullptr ? std::get_if<fastgltf::sources::Array>(data) : nullptr;
                            if (bytes == nullptr || recorded == nullptr)
                            {
                                errors.Fail("Could not identify the glTF dependency " + std::string(text),
                                            SceneImportStatus::SourceUnavailable);
                            }
                            else if (bytes->bytes.size() != recorded->size)
                            {
                                errors.Fail("The glTF dependency " + std::string(text) + " changed while it was read",
                                            SceneImportStatus::SourceUnavailable);
                            }
                            else if (collection[0] == 'b'
                                     && std::find(progress.imageFiles.begin(), progress.imageFiles.end(), resolved)
                                            == progress.imageFiles.end())
                            {
                                progress.CompleteFile(resolved);
                            }
                        }
                    }
                }
                ++index;
            }
        }
    }
}

void GltfAssetReader::ImportFile(const std::filesystem::path& filePath, bool binary, Scene* pScene)
{
    ZENASSET_MEASURE_BEGIN(sourceParsing, SourceAcquireParse);
    fastgltf::Expected<fastgltf::MappedGltfFile> gltfFile = fastgltf::MappedGltfFile::FromPath(filePath);

    if (!bool(gltfFile))
    {
        m_errors.Fail((std::string("Failed to open glTF file: ") + std::string(fastgltf::getErrorMessage(gltfFile.error()))));
    }
    else
    {
        const fastgltf::span<std::byte> fileBytes = static_cast<fastgltf::span<std::byte>>(gltfFile.get());

        size_t jsonOffset                         = 0;

        size_t jsonLength                         = fileBytes.size();

        if (binary && fileBytes.size() >= 20)
        {
            uint32_t length = 0;

            std::memcpy(&length, fileBytes.data() + 12, sizeof(length));

            jsonOffset = 20;

            jsonLength = length;
        }

        if (jsonOffset > fileBytes.size() || jsonLength > fileBytes.size() - jsonOffset)
        {
            m_errors.Fail("Invalid glTF JSON chunk length");
        }
        else
        {
            m_sourceJson.assign(reinterpret_cast<const char*>(fileBytes.data() + jsonOffset), jsonLength);

            // glTF integer properties may use decimal or exponent notation. fastgltf's
            // uint64 reader requires integer tokens, so normalize exact integral values
            // lexically without rounding fractional values or changing quoted strings.
            m_json                             = NormalizeIntegralJsonNumbers(m_sourceJson);

            std::vector<std::byte> parserBytes = PrepareExtensionParserInput(m_json, m_sourceJson, fileBytes, binary, m_errors);

            if (!m_errors.Failed())
            {
                // simdjson reads SIMDJSON_PADDING bytes past the JSON. A mapped file cannot
                // guarantee that padding at a page boundary; copy into fastgltf's padded buffer.
                fastgltf::Expected<fastgltf::GltfDataBuffer> input =
                    fastgltf::GltfDataBuffer::FromBytes(parserBytes.empty() ? fileBytes.data() : parserBytes.data(),
                                                        parserBytes.empty() ? fileBytes.size() : parserBytes.size());

                if (input.error() != fastgltf::Error::None)
                {
                    m_errors.Fail("Failed to allocate padded glTF parser input");
                }
                else
                {
                    fastgltf::GltfDataBuffer parserData = std::move(input.get());

                    RecordGltfDependencies(m_sourceJson, nullptr, filePath, *pScene, m_errors, m_fileProgress);
                    if (!m_errors.Failed() && m_fileProgress.control != nullptr)
                    {
                        m_fileProgress.control->FinishFileDiscovery();
                    }

                    fastgltf::Expected<fastgltf::Asset> loadedAsset =
                        m_gltfParser.loadGltf(parserData, filePath.parent_path(), m_loadOptions);

                    if (loadedAsset.error() != fastgltf::Error::None)
                    {
                        m_errors.Fail((std::string("Failed to load glTF: ")
                                       + std::string(fastgltf::getErrorMessage(loadedAsset.error()))));
                    }
                    else
                    {
                        m_gltfAsset = std::move(loadedAsset.get());

                        RecordGltfDependencies(m_sourceJson, &m_gltfAsset, filePath, *pScene, m_errors, m_fileProgress);

                        // The asset may view parserData, so the import finishes in this scope.
                        ZENASSET_MEASURE_END(sourceParsing);
                        ImportAsset(pScene);
                        if (!m_errors.Failed())
                        {
                            // Dependencies loaded by fastgltf but unused by the selected scene still count once.
                            for (const SceneDependency& dependency : pScene->dependencies)
                            {
                                m_fileProgress.CompleteFile(dependency.path);
                            }
                        }
                    }
                }
            }
        }
    }
}

void GltfAssetReader::ImportAsset(Scene* pScene)
{
    ZENASSET_MEASURE_SCOPE(SourceConvert);
    if (m_settings.sceneIndex != UINT32_MAX)
    {
        if (m_settings.sceneIndex >= m_gltfAsset.scenes.size())
        {
            m_errors.Fail("Selected source scene does not exist");
        }
        else
        {
            m_gltfAsset.defaultScene = m_settings.sceneIndex;
        }
    }

    LoadNodeProperties(m_json, m_gltfAsset, m_errors);

    if (!m_errors.Failed())
    {
        DecodeMeshoptBuffers(m_gltfAsset, m_errors);
    }

    if (!m_errors.Failed())
    {
        DecodeDracoMeshes(m_gltfAsset, m_errors);
    }

    if (!m_errors.Failed())
    {
        const fastgltf::Error validation = ValidateSceneAsset(m_gltfAsset);

        if (validation != fastgltf::Error::None)
        {
            m_errors.Fail((std::string("Invalid glTF scene: ") + std::string(fastgltf::getErrorMessage(validation))));
        }
    }

    if (!m_errors.Failed())
    {
        pScene->name = m_name;

        LoadGltfSamplers(pScene);

        if (!m_errors.Failed())
        {
            m_errors.SetStage(SceneImportStage::DecodingTextures);
            LoadGltfTextures(pScene);
        }

        if (!m_errors.Failed())
        {
            LoadGltfMaterials(pScene);
        }

        if (!m_errors.Failed())
        {
            m_errors.SetStage(SceneImportStage::ProcessingMeshes);
            LoadGltfMeshes(pScene);
        }

        if (!m_errors.Failed())
        {
            m_errors.SetStage(SceneImportStage::BuildingScene);
            LoadGltfAssetData(pScene);
        }

        if (!m_errors.Failed())
        {
            LoadGltfRenderableNodes(pScene);
            for (AnimationAsset& animation : pScene->animations)
            {
                for (AnimationChannel& channel : animation.channels)
                {
                    if (channel.path == AnimationPath::Property
                        && !CompileGltfAnimationTarget(*pScene, channel.sourceTarget, channel.target))
                    {
                        m_errors.Fail("Unsupported or invalid glTF animation target: " + channel.sourceTarget);
                    }
                }
            }
        }
    }
}

void GltfAssetReader::LoadGltfSamplers(Scene* pScene)
{ // Load Samplers
    std::vector<SamplerAsset> samplers;

    samplers.reserve(m_gltfAsset.samplers.size());

    using SamplerKey = std::tuple<TextureFilter, TextureFilter, TextureFilter, SamplerAddressMode, SamplerAddressMode, bool>;

    std::map<SamplerKey, uint32_t> canonicalSamplers;

    m_samplerIndices.resize(m_gltfAsset.samplers.size());

    uint32_t sourceIndex = 0;

    for (const fastgltf::Sampler& s : m_gltfAsset.samplers)
    {
        SamplerAsset sampler;
        sampler.name           = std::string(s.name);
        SamplerAsset* pSampler = &sampler;

        // set props
        pSampler->minFilter              = FromFastGltfFilter(s.minFilter);

        pSampler->magFilter              = FromFastGltfFilter(s.magFilter);

        const fastgltf::Filter minFilter = s.minFilter.value_or(fastgltf::Filter::Linear);

        pSampler->useMipmaps             = minFilter != fastgltf::Filter::Nearest && minFilter != fastgltf::Filter::Linear;

        pSampler->mipFilter =
            minFilter == fastgltf::Filter::NearestMipMapNearest || minFilter == fastgltf::Filter::LinearMipMapNearest
                ? TextureFilter::Nearest
                : TextureFilter::Linear;

        pSampler->wrapS = FromFastGltfWrap(s.wrapS);

        pSampler->wrapT = FromFastGltfWrap(s.wrapT);

        const SamplerKey key(pSampler->minFilter, pSampler->magFilter, pSampler->mipFilter, pSampler->wrapS, pSampler->wrapT,
                             pSampler->useMipmaps);

        const std::map<SamplerKey, uint32_t>::const_iterator found = canonicalSamplers.find(key);

        if (found == canonicalSamplers.end())
        {
            const uint32_t canonical = static_cast<uint32_t>(samplers.size());

            canonicalSamplers.emplace(key, canonical);

            samplers.push_back(std::move(sampler));

            m_samplerIndices[sourceIndex] = canonical;
        }
        else
        {
            m_samplerIndices[sourceIndex] = found->second;
        }

        ++sourceIndex;
    }

    pScene->samplers = std::move(samplers);
}

using TextureRole = std::pair<const fastgltf::TextureInfo*, bool>;

static std::vector<TextureRole> GetMaterialTextureRoles(const fastgltf::Material& material)
{
    std::vector<TextureRole> roles;

    roles.push_back({material.pbrData.baseColorTexture ? &*material.pbrData.baseColorTexture : nullptr, true});

    roles.push_back({material.emissiveTexture ? &*material.emissiveTexture : nullptr, true});

    roles.push_back({material.pbrData.metallicRoughnessTexture ? &*material.pbrData.metallicRoughnessTexture : nullptr, false});

    roles.push_back({material.normalTexture ? &*material.normalTexture : nullptr, false});

    roles.push_back({material.occlusionTexture ? &*material.occlusionTexture : nullptr, false});

    if (material.specular)
    {
        roles.push_back({material.specular->specularTexture ? &*material.specular->specularTexture : nullptr, false});

        roles.push_back({material.specular->specularColorTexture ? &*material.specular->specularColorTexture : nullptr, true});
    }

    if (material.specularGlossiness)
    {
        roles.push_back(
            {material.specularGlossiness->diffuseTexture ? &*material.specularGlossiness->diffuseTexture : nullptr, true});

        roles.push_back({material.specularGlossiness->specularGlossinessTexture
                             ? &*material.specularGlossiness->specularGlossinessTexture
                             : nullptr,
                         true});
    }

    if (material.clearcoat)
    {
        roles.push_back({material.clearcoat->clearcoatTexture ? &*material.clearcoat->clearcoatTexture : nullptr, false});

        roles.push_back(
            {material.clearcoat->clearcoatRoughnessTexture ? &*material.clearcoat->clearcoatRoughnessTexture : nullptr, false});

        roles.push_back(
            {material.clearcoat->clearcoatNormalTexture ? &*material.clearcoat->clearcoatNormalTexture : nullptr, false});
    }

    if (material.sheen)
    {
        roles.push_back({material.sheen->sheenColorTexture ? &*material.sheen->sheenColorTexture : nullptr, true});

        roles.push_back({material.sheen->sheenRoughnessTexture ? &*material.sheen->sheenRoughnessTexture : nullptr, false});
    }

    if (material.transmission)
    {
        roles.push_back(
            {material.transmission->transmissionTexture ? &*material.transmission->transmissionTexture : nullptr, false});
    }

    if (material.volume)
    {
        roles.push_back({material.volume->thicknessTexture ? &*material.volume->thicknessTexture : nullptr, false});
    }

    if (material.iridescence)
    {
        roles.push_back(
            {material.iridescence->iridescenceTexture ? &*material.iridescence->iridescenceTexture : nullptr, false});

        roles.push_back(
            {material.iridescence->iridescenceThicknessTexture ? &*material.iridescence->iridescenceThicknessTexture : nullptr,
             false});
    }

    if (material.anisotropy)
    {
        roles.push_back({material.anisotropy->anisotropyTexture ? &*material.anisotropy->anisotropyTexture : nullptr, false});
    }

    if (material.diffuseTransmission)
    {
        roles.push_back({material.diffuseTransmission->diffuseTransmissionTexture
                             ? &*material.diffuseTransmission->diffuseTransmissionTexture
                             : nullptr,
                         false});

        roles.push_back({material.diffuseTransmission->diffuseTransmissionColorTexture
                             ? &*material.diffuseTransmission->diffuseTransmissionColorTexture
                             : nullptr,
                         true});
    }

    return roles;
}

// An invalid texture index records an error and returns an empty binding.
static MaterialTextureBinding LoadTextureBinding(const fastgltf::TextureInfo*      source,
                                                 bool                              color,
                                                 const std::vector<TextureAsset>&  textures,
                                                 const std::vector<uint32_t>&      textureIndices,
                                                 const std::vector<uint32_t>&      linearIndices,
                                                 const std::map<size_t, uint32_t>& texCoordIndices,
                                                 ImportErrors&                     errors)
{
    MaterialTextureBinding binding;

    if (source != nullptr && source->textureIndex >= linearIndices.size())
    {
        errors.Fail("glTF material references an invalid texture index");
    }
    else if (source != nullptr)
    {
        binding.texture = TextureId{color ? textureIndices[source->textureIndex] : linearIndices[source->textureIndex]};

        const size_t coordinateSet =
            source->transform && source->transform->texCoordIndex ? *source->transform->texCoordIndex : source->texCoordIndex;

        const std::map<size_t, uint32_t>::const_iterator coordinate = texCoordIndices.find(coordinateSet);

        binding.texCoord = coordinate == texCoordIndices.end() ? 0 : coordinate->second;

        if (source->transform != nullptr)
        {
            const fastgltf::TextureTransform& transform = *source->transform;

            binding.transform.uvOffset                  = MakeVec2(transform.uvOffset.data());

            binding.transform.uvScale                   = MakeVec2(transform.uvScale.data());

            binding.transform.uvRotation                = transform.rotation;

            const float cosine                          = std::cos(transform.rotation);

            const float sine                            = std::sin(transform.rotation);

            binding.transform.row0 =
                Vec4(cosine * transform.uvScale[0], -sine * transform.uvScale[1], transform.uvOffset[0], 0);

            binding.transform.row1 = Vec4(sine * transform.uvScale[0], cosine * transform.uvScale[1], transform.uvOffset[1], 0);
        }
    }

    return binding;
}

static void LoadMaterialFeatures(const fastgltf::Material&         source,
                                 MaterialAsset&                    material,
                                 const std::vector<TextureAsset>&  textures,
                                 const std::vector<uint32_t>&      textureIndices,
                                 const std::vector<uint32_t>&      linearIndices,
                                 const std::map<size_t, uint32_t>& texCoordIndices,
                                 ImportErrors&                     errors)
{
    MaterialFeatures& features = material.features;

    features.ior               = source.ior;

    features.dispersion        = source.dispersion;

    if (source.specular)
    {
        features.specular      = source.specular->specularFactor;

        features.specularColor = MakeVec3(source.specular->specularColorFactor.data());

        features.specularTexture =
            LoadTextureBinding(source.specular->specularTexture ? &*source.specular->specularTexture : nullptr, false, textures,
                               textureIndices, linearIndices, texCoordIndices, errors);

        features.specularColorTexture =
            LoadTextureBinding(source.specular->specularColorTexture ? &*source.specular->specularColorTexture : nullptr, true,
                               textures, textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.specularGlossiness)
    {
        material.pbrWorkflows.metallicRoughness  = false;

        material.pbrWorkflows.specularGlossiness = true;

        material.extension.diffuseFactor         = MakeVec4(source.specularGlossiness->diffuseFactor.data());

        material.extension.specularFactor        = MakeVec3(source.specularGlossiness->specularFactor.data());

        features.glossiness                      = source.specularGlossiness->glossinessFactor;

        features.diffuseTexture                  = LoadTextureBinding(
            source.specularGlossiness->diffuseTexture ? &*source.specularGlossiness->diffuseTexture : nullptr, true, textures,
            textureIndices, linearIndices, texCoordIndices, errors);

        features.specularGlossinessTexture = LoadTextureBinding(
            source.specularGlossiness->specularGlossinessTexture ? &*source.specularGlossiness->specularGlossinessTexture
                                                                 : nullptr,
            true, textures, textureIndices, linearIndices, texCoordIndices, errors);

        material.extension.pDiffuseTexture            = features.diffuseTexture.texture;

        material.extension.pSpecularGlossinessTexture = features.specularGlossinessTexture.texture;
    }

    if (source.clearcoat)
    {
        features.clearcoat          = source.clearcoat->clearcoatFactor;

        features.clearcoatRoughness = source.clearcoat->clearcoatRoughnessFactor;

        features.clearcoatTexture =
            LoadTextureBinding(source.clearcoat->clearcoatTexture ? &*source.clearcoat->clearcoatTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices, errors);

        features.clearcoatRoughnessTexture = LoadTextureBinding(
            source.clearcoat->clearcoatRoughnessTexture ? &*source.clearcoat->clearcoatRoughnessTexture : nullptr, false,
            textures, textureIndices, linearIndices, texCoordIndices, errors);

        features.clearcoatNormalTexture =
            LoadTextureBinding(source.clearcoat->clearcoatNormalTexture ? &*source.clearcoat->clearcoatNormalTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices, errors);

        features.clearcoatNormalTexture.scale =
            source.clearcoat->clearcoatNormalTexture ? source.clearcoat->clearcoatNormalTexture->scale : 1.0f;
    }

    if (source.sheen)
    {
        features.sheenColor     = MakeVec3(source.sheen->sheenColorFactor.data());

        features.sheenRoughness = source.sheen->sheenRoughnessFactor;

        features.sheenColorTexture =
            LoadTextureBinding(source.sheen->sheenColorTexture ? &*source.sheen->sheenColorTexture : nullptr, true, textures,
                               textureIndices, linearIndices, texCoordIndices, errors);

        features.sheenRoughnessTexture =
            LoadTextureBinding(source.sheen->sheenRoughnessTexture ? &*source.sheen->sheenRoughnessTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.transmission)
    {
        features.transmission = source.transmission->transmissionFactor;

        features.transmissionTexture =
            LoadTextureBinding(source.transmission->transmissionTexture ? &*source.transmission->transmissionTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.volume)
    {
        features.thickness           = source.volume->thicknessFactor;

        features.attenuationColor    = MakeVec3(source.volume->attenuationColor.data());

        features.attenuationDistance = source.volume->attenuationDistance;

        features.thicknessTexture =
            LoadTextureBinding(source.volume->thicknessTexture ? &*source.volume->thicknessTexture : nullptr, false, textures,
                               textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.iridescence)
    {
        features.iridescence             = source.iridescence->iridescenceFactor;

        features.iridescenceIor          = source.iridescence->iridescenceIor;

        features.iridescenceThicknessMin = source.iridescence->iridescenceThicknessMinimum;

        features.iridescenceThicknessMax = source.iridescence->iridescenceThicknessMaximum;

        features.iridescenceTexture =
            LoadTextureBinding(source.iridescence->iridescenceTexture ? &*source.iridescence->iridescenceTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices, errors);

        features.iridescenceThicknessTexture = LoadTextureBinding(
            source.iridescence->iridescenceThicknessTexture ? &*source.iridescence->iridescenceThicknessTexture : nullptr,
            false, textures, textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.anisotropy)
    {
        features.anisotropy         = source.anisotropy->anisotropyStrength;

        features.anisotropyRotation = source.anisotropy->anisotropyRotation;

        features.anisotropyTexture =
            LoadTextureBinding(source.anisotropy->anisotropyTexture ? &*source.anisotropy->anisotropyTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices, errors);
    }

    if (source.diffuseTransmission)
    {
        features.diffuseTransmission        = source.diffuseTransmission->diffuseTransmissionFactor;

        features.diffuseTransmissionColor   = MakeVec3(source.diffuseTransmission->diffuseTransmissionColorFactor.data());

        features.diffuseTransmissionTexture = LoadTextureBinding(
            source.diffuseTransmission->diffuseTransmissionTexture ? &*source.diffuseTransmission->diffuseTransmissionTexture
                                                                   : nullptr,
            false, textures, textureIndices, linearIndices, texCoordIndices, errors);

        features.diffuseTransmissionColorTexture =
            LoadTextureBinding(source.diffuseTransmission->diffuseTransmissionColorTexture
                                   ? &*source.diffuseTransmission->diffuseTransmissionColorTexture
                                   : nullptr,
                               true, textures, textureIndices, linearIndices, texCoordIndices, errors);
    }
}

static fastgltf::span<const std::byte> GetImageBytes(const fastgltf::Asset&  asset,
                                                     const fastgltf::Image&  gltfImage,
                                                     std::vector<std::byte>& fileBytes,
                                                     ImportErrors&           errors)
{
    fastgltf::span<const std::byte> bytes;

    if (const fastgltf::sources::URI* file = std::get_if<fastgltf::sources::URI>(&gltfImage.data))
    {
        const std::string path(file->uri.path().begin(), file->uri.path().end());

        std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);

        if (stream)
        {
            const std::streamoff length = stream.tellg();

            if (length >= 0)
            {
                fileBytes.resize(static_cast<size_t>(length));

                stream.seekg(0);

                stream.read(reinterpret_cast<char*>(fileBytes.data()), length);

                if (stream)
                {
                    bytes = fastgltf::span<const std::byte>(fileBytes.data(), fileBytes.size());
                }
            }
        }
    }
    else if (const fastgltf::sources::Array* array = std::get_if<fastgltf::sources::Array>(&gltfImage.data))
    {
        bytes = fastgltf::span<const std::byte>(array->bytes.data(), array->bytes.size());
    }
    else if (const fastgltf::sources::Vector* vector = std::get_if<fastgltf::sources::Vector>(&gltfImage.data))
    {
        bytes = fastgltf::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
    }
    else if (const fastgltf::sources::ByteView* byteView = std::get_if<fastgltf::sources::ByteView>(&gltfImage.data))
    {
        bytes = byteView->bytes;
    }
    else if (const fastgltf::sources::BufferView* view = std::get_if<fastgltf::sources::BufferView>(&gltfImage.data))
    {
        if (view->bufferViewIndex < asset.bufferViews.size())
        {
            const fastgltf::BufferView& bufferView = asset.bufferViews[view->bufferViewIndex];

            bytes = GetBufferBytes(asset, bufferView.bufferIndex, bufferView.byteOffset, bufferView.byteLength, errors);
        }
    }

    return bytes;
}

// Runs on texture workers. A failure records an error and returns null.
std::shared_ptr<const ImagePixels> GltfAssetReader::DecodeGltfImage(uint32_t imageIndex)
{
    std::shared_ptr<const ImagePixels> result;
    if (!m_errors.Failed())
    {
        std::vector<std::byte>                fileBytes;
        const fastgltf::span<const std::byte> bytes =
            GetImageBytes(m_gltfAsset, m_gltfAsset.images[imageIndex], fileBytes, m_errors);
        std::shared_ptr<ImagePixels> image(new ImagePixels());
        ImportErrors                 decoding;
        decoding.Reset(nullptr, m_errors.GetImageBudget());
        image->bytes = DecodeImagePixels(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), image->width,
                                         image->height, decoding, &image->mips);
        if (m_errors.Failed() || decoding.Failed() || image->bytes.empty() || image->width == 0 || image->height == 0)
        {
            m_errors.Fail("Failed to decode glTF image " + std::to_string(imageIndex)
                              + (decoding.Failed() ? ": " + decoding.Message() : std::string()),
                          decoding.Failed() ? decoding.Status() : SceneImportStatus::InvalidData);
        }
        else
        {
            result = std::move(image);
            m_fileProgress.CompleteImage(imageIndex);
        }
    }
    return result;
}

static std::vector<uint32_t> GetAdditionalLinearTextures(const std::string& json)
{
    std::vector<uint32_t> textures;

    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array materials;

    if (parser.parse(json).get(document) == simdjson::SUCCESS
        && document["materials"].get_array().get(materials) == simdjson::SUCCESS)
    {
        for (simdjson::dom::element material : materials)
        {
            uint64_t texture = 0;

            if (material.at_pointer("/extensions/KHR_materials_retroreflection/retroreflectionTexture/index")
                    .get_uint64()
                    .get(texture)
                == simdjson::SUCCESS)
            {
                textures.push_back(static_cast<uint32_t>(texture));
            }
        }
    }

    return textures;
}

void GltfAssetReader::LoadGltfTextures(Scene* pScene)
{
    ZENASSET_MEASURE_SCOPE(SourceImages);
    const size_t numSourceTextures = m_gltfAsset.textures.size();

    std::vector<uint8_t> colorUsage(numSourceTextures, 0);

    std::vector<uint8_t> linearUsage(numSourceTextures, 0);

    for (const fastgltf::Material& material : m_gltfAsset.materials)
    {
        const std::vector<TextureRole> roles = GetMaterialTextureRoles(material);

        for (const TextureRole& role : roles)
        {
            if (role.first != nullptr && role.first->textureIndex >= numSourceTextures)
            {
                m_errors.Fail("glTF material references an invalid texture");
            }
            else if (role.first != nullptr)
            {
                (role.second ? colorUsage : linearUsage)[role.first->textureIndex] = 1;
            }
        }
    }

    for (uint32_t texture : GetAdditionalLinearTextures(m_json))
    {
        if (texture >= numSourceTextures)
        {
            m_errors.Fail("glTF material extension references an invalid texture");
        }
        else
        {
            linearUsage[texture] = 1;
        }
    }

    using TextureKey = std::tuple<size_t, int, PixelFormat>;

    std::map<TextureKey, uint32_t> canonicalTextures;

    std::vector<std::pair<uint32_t, uint32_t>> linearClones;

    m_textureIndices.resize(numSourceTextures);

    m_linearTextureIndices.resize(numSourceTextures);

    m_textureFormats.resize(numSourceTextures);

    m_uniqueTextureIndices.clear();

    // glTF texture indices remain source bindings. Decode and upload each image,
    // sampler and color-space interpretation once, then map source bindings to it.
    for (uint32_t index = 0; index < numSourceTextures && !m_errors.Failed(); ++index)
    {
        const fastgltf::Texture& texture        = m_gltfAsset.textures[index];

        const fastgltf::Optional<size_t> source = texture.basisuImageIndex ? texture.basisuImageIndex
                                                : texture.webpImageIndex   ? texture.webpImageIndex
                                                                           : texture.imageIndex;

        if (!source || *source >= m_gltfAsset.images.size())
        {
            m_errors.Fail("glTF texture has no valid image source");
        }
        else
        {
            const int sampler        = texture.samplerIndex ? static_cast<int>(m_samplerIndices[*texture.samplerIndex]) : -1;

            const PixelFormat format = colorUsage[index] ? PixelFormat::RGBA8Srgb : PixelFormat::RGBA8Unorm;

            const TextureKey key(*source, sampler, format);

            std::map<TextureKey, uint32_t>::const_iterator found = canonicalTextures.find(key);

            if (found == canonicalTextures.end())
            {
                const uint32_t canonical = static_cast<uint32_t>(m_uniqueTextureIndices.size());

                canonicalTextures.emplace(key, canonical);

                m_uniqueTextureIndices.push_back(index);

                m_textureIndices[index] = canonical;
            }
            else
            {
                m_textureIndices[index] = found->second;
            }

            m_textureFormats[index]       = format;

            m_linearTextureIndices[index] = m_textureIndices[index];
        }
    }

    if (!m_errors.Failed())
    {
        const size_t numTextures = m_uniqueTextureIndices.size();

        for (uint32_t index = 0; index < numSourceTextures; ++index)
        {
            if (colorUsage[index] && linearUsage[index])
            {
                const fastgltf::Texture& texture = m_gltfAsset.textures[index];

                const size_t source              = texture.basisuImageIndex ? *texture.basisuImageIndex
                                                 : texture.webpImageIndex   ? *texture.webpImageIndex
                                                                            : *texture.imageIndex;

                const int sampler = texture.samplerIndex ? static_cast<int>(m_samplerIndices[*texture.samplerIndex]) : -1;

                const TextureKey key(source, sampler, PixelFormat::RGBA8Unorm);

                const std::map<TextureKey, uint32_t>::const_iterator found = canonicalTextures.find(key);

                if (found == canonicalTextures.end())
                {
                    const uint32_t canonical = static_cast<uint32_t>(numTextures + linearClones.size());

                    canonicalTextures.emplace(key, canonical);

                    linearClones.push_back({m_textureIndices[index], canonical});

                    m_linearTextureIndices[index] = canonical;
                }
                else
                {
                    m_linearTextureIndices[index] = found->second;
                }
            }
        }

        std::vector<uint32_t>                                        imageIndices(m_gltfAsset.images.size(), UINT32_MAX);
        std::vector<std::future<std::shared_ptr<const ImagePixels>>> futures;
        for (uint32_t index : m_uniqueTextureIndices)
        {
            const fastgltf::Texture& source     = m_gltfAsset.textures[index];
            const uint32_t           imageIndex = static_cast<uint32_t>(source.basisuImageIndex ? *source.basisuImageIndex
                                                                        : source.webpImageIndex ? *source.webpImageIndex
                                                                                                : *source.imageIndex);
            if (imageIndices[imageIndex] == UINT32_MAX)
            {
                imageIndices[imageIndex] = static_cast<uint32_t>(pScene->images.size());
                ImageAsset image;
                image.name = std::string(m_gltfAsset.images[imageIndex].name);
                pScene->images.push_back(std::move(image));
                futures.emplace_back(m_workers->Push(&GltfAssetReader::DecodeGltfImage, this, imageIndex));
            }
            TextureAsset texture;
            texture.name    = m_name + "Texture_" + std::to_string(index);
            texture.image   = ImageId{imageIndices[imageIndex]};
            texture.sampler = SamplerId{source.samplerIndex ? m_samplerIndices[*source.samplerIndex] : UINT32_MAX};
            texture.format  = m_textureFormats[index];
            pScene->textures.push_back(std::move(texture));
        }
        // Drain all workers even after failure/cancellation, before releasing parser bytes.
        for (size_t index = 0; index < futures.size(); ++index)
        {
            pScene->images[index].pixels = futures[index].get();
        }
        if (!m_errors.Failed())
        {
            for (const std::pair<uint32_t, uint32_t>& clone : linearClones)
            {
                TextureAsset texture  = pScene->textures[clone.first];
                texture.name         += "_linear";
                texture.format        = PixelFormat::RGBA8Unorm;
                texture.linearSource  = TextureId{clone.first};
                pScene->textures.push_back(std::move(texture));
            }
        }
    }
}

// An absent vector keeps the existing values; a malformed one records an error.
static void ReadJsonVector(const simdjson::dom::element& object,
                           const char*                   key,
                           float*                        values,
                           size_t                        components,
                           ImportErrors&                 errors)
{
    simdjson::dom::array array;

    const bool present = object[key].get_array().get(array) == simdjson::SUCCESS;

    if (present && array.size() != components)
    {
        errors.Fail("glTF extension vector has an invalid component count");
    }
    else if (present)
    {
        size_t component = 0;

        for (simdjson::dom::element entry : array)
        {
            double value = 0.0;

            if (entry.get_double().get(value) != simdjson::SUCCESS || !std::isfinite(value))
            {
                errors.Fail("glTF extension vector has a nonnumeric or nonfinite value");
            }
            else
            {
                values[component] = static_cast<float>(value);
            }

            ++component;
        }
    }
}

static void LoadAdditionalMaterialFeatures(const simdjson::dom::element&     document,
                                           MaterialAsset&                    material,
                                           const std::vector<TextureAsset>&  textures,
                                           const std::vector<uint32_t>&      textureIndices,
                                           const std::vector<uint32_t>&      linearIndices,
                                           const std::map<size_t, uint32_t>& texCoordIndices,
                                           ImportErrors&                     errors)
{
    const std::string path = "/extensions/";

    simdjson::dom::element scatter;

    if (document.at_pointer(path + "KHR_materials_volume_scatter").get(scatter) == simdjson::SUCCESS)
    {
        ReadJsonVector(scatter, "multiscatterColorFactor", &material.features.multiscatterColor.x, 3, errors);
    }

    simdjson::dom::element retro;

    if (document.at_pointer(path + "KHR_materials_retroreflection").get(retro) == simdjson::SUCCESS)
    {
        double factor = 0.0;

        if (retro["retroreflectionFactor"].get_double().get(factor) == simdjson::SUCCESS)
        {
            material.features.retroreflection = static_cast<float>(factor);
        }

        simdjson::dom::element texture;

        if (retro["retroreflectionTexture"].get(texture) == simdjson::SUCCESS)
        {
            uint64_t textureIndex = 0;

            if (texture["index"].get_uint64().get(textureIndex) != simdjson::SUCCESS)
            {
                errors.Fail("Retroreflection texture requires an image texture index");
            }
            else
            {
                fastgltf::TextureInfo binding;

                binding.textureIndex = static_cast<size_t>(textureIndex);

                uint64_t coordinates = 0;

                texture["texCoord"].get_uint64().get(coordinates);

                binding.texCoordIndex = static_cast<size_t>(coordinates);

                simdjson::dom::element transform;

                if (texture.at_pointer("/extensions/KHR_texture_transform").get(transform) == simdjson::SUCCESS)
                {
                    binding.transform = std::make_unique<fastgltf::TextureTransform>();

                    ReadJsonVector(transform, "offset", binding.transform->uvOffset.data(), 2, errors);

                    ReadJsonVector(transform, "scale", binding.transform->uvScale.data(), 2, errors);

                    double rotation = 0.0;

                    transform["rotation"].get_double().get(rotation);

                    binding.transform->rotation = static_cast<float>(rotation);

                    if (transform["texCoord"].get_uint64().get(coordinates) == simdjson::SUCCESS)
                    {
                        binding.transform->texCoordIndex = static_cast<size_t>(coordinates);
                    }
                }

                material.features.retroreflectionTexture =
                    LoadTextureBinding(&binding, false, textures, textureIndices, linearIndices, texCoordIndices, errors);
            }
        }
    }
}

void GltfAssetReader::LoadGltfMaterials(Scene* pScene)
{
    m_texCoordIndices.clear();

    if (m_errors.ReserveGeometryBytes(2, sizeof(std::map<size_t, uint32_t>::value_type)))
    {
        m_texCoordIndices.emplace(0, 0);
        m_texCoordIndices.emplace(1, 1);
    }

    for (const fastgltf::Mesh& mesh : m_gltfAsset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            std::vector<const fastgltf::Attribute*> coordinateAttributes;

            for (const fastgltf::Attribute& attribute : primitive.attributes)
            {
                if (ReserveGeometryVector(coordinateAttributes, coordinateAttributes.size() + 1, m_errors))
                {
                    coordinateAttributes.push_back(&attribute);
                }
            }

            for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& target : primitive.targets)
            {
                for (const fastgltf::Attribute& attribute : target)
                {
                    if (ReserveGeometryVector(coordinateAttributes, coordinateAttributes.size() + 1, m_errors))
                    {
                        coordinateAttributes.push_back(&attribute);
                    }
                }
            }

            for (const fastgltf::Attribute* sourceAttribute : coordinateAttributes)
            {
                const fastgltf::Attribute& attribute = *sourceAttribute;

                if (attribute.name.starts_with("TEXCOORD_"))
                {
                    size_t source                       = 0;

                    const char* begin                   = attribute.name.data() + 9;

                    const char* end                     = attribute.name.data() + attribute.name.size();

                    const std::from_chars_result parsed = std::from_chars(begin, end, source);

                    if (parsed.ec != std::errc{} || parsed.ptr != end)
                    {
                        m_errors.Fail("Invalid glTF texture-coordinate semantic index");
                    }
                    else
                    {
                        if (!m_texCoordIndices.contains(source)
                            && m_errors.ReserveGeometryBytes(1, sizeof(std::map<size_t, uint32_t>::value_type)))
                        {
                            m_texCoordIndices.try_emplace(source, 0);
                        }
                    }
                }
            }
        }
    }

    std::vector<uint64_t>& texCoordSets = pScene->vertexTexCoordSets;

    for (std::map<size_t, uint32_t>::value_type& coordinate : m_texCoordIndices)
    {
        coordinate.second = static_cast<uint32_t>(texCoordSets.size());

        if (ReserveGeometryVector(texCoordSets, texCoordSets.size() + 1, m_errors))
        {
            texCoordSets.push_back(static_cast<uint64_t>(coordinate.first));
        }
    }

    simdjson::dom::parser jsonParser;

    simdjson::dom::element document;

    if (jsonParser.parse(m_json).get(document) != simdjson::SUCCESS)
    {
        m_errors.Fail("Failed to parse glTF material extension metadata");
    }

    std::vector<simdjson::dom::element> sourceMaterials;

    simdjson::dom::array sourceArray;

    if (!m_errors.Failed() && document["materials"].get_array().get(sourceArray) == simdjson::SUCCESS)
    {
        sourceMaterials.reserve(sourceArray.size());

        for (simdjson::dom::element material : sourceArray)
        {
            sourceMaterials.push_back(material);
        }
    }

    if (sourceMaterials.size() != m_gltfAsset.materials.size())
    {
        m_errors.Fail("glTF material metadata does not match parsed materials");
    }

    if (!m_errors.Failed())
    {
        pScene->materials.resize(m_gltfAsset.materials.size());
        for (uint32_t materialIndex = 0; materialIndex < m_gltfAsset.materials.size(); ++materialIndex)
        {
            const fastgltf::Material& mat          = m_gltfAsset.materials[materialIndex];
            MaterialAsset&            material     = pScene->materials[materialIndex];
            material.name                          = std::string(mat.name);
            material.doubleSided                   = mat.doubleSided;
            material.alphaCutoff                   = mat.alphaCutoff;
            material.baseColorFactor               = MakeVec4(mat.pbrData.baseColorFactor.data());
            material.roughnessFactor               = mat.pbrData.roughnessFactor;
            material.metallicFactor                = mat.pbrData.metallicFactor;
            material.normalScale                   = mat.normalTexture ? mat.normalTexture->scale : 1.0f;
            material.occlusionStrength             = mat.occlusionTexture ? mat.occlusionTexture->strength : 1.0f;
            material.emissiveStrength              = mat.emissiveStrength;
            material.emissiveFactor                = Vec4(MakeVec3(mat.emissiveFactor.data()), 0.0f);
            material.alphaMode                     = mat.alphaMode == fastgltf::AlphaMode::Blend ? AlphaMode::Blend
                                                   : mat.alphaMode == fastgltf::AlphaMode::Mask  ? AlphaMode::Mask
                                                                                                 : AlphaMode::Opaque;
            material.unlit                         = mat.unlit;
            const fastgltf::TextureInfo* sources[] = {
                mat.pbrData.baseColorTexture ? &*mat.pbrData.baseColorTexture : nullptr,
                mat.pbrData.metallicRoughnessTexture ? &*mat.pbrData.metallicRoughnessTexture : nullptr,
                mat.normalTexture ? &*mat.normalTexture : nullptr, mat.occlusionTexture ? &*mat.occlusionTexture : nullptr,
                mat.emissiveTexture ? &*mat.emissiveTexture : nullptr};
            TextureId* bindings[] = {&material.m_pBaseColorTexture, &material.m_pMetallicRoughnessTexture,
                                     &material.m_pNormalTexture, &material.m_pOcclusionTexture, &material.m_pEmissiveTexture};
            uint32_t*  coordinates[] = {&material.texCoordSets.baseColor, &material.texCoordSets.metallicRoughness,
                                        &material.texCoordSets.normal, &material.texCoordSets.occlusion,
                                        &material.texCoordSets.emissive};
            for (uint32_t index = 0; index < 5; ++index)
            {
                const MaterialTextureBinding binding =
                    LoadTextureBinding(sources[index], index == 0 || index == 4, pScene->textures, m_textureIndices,
                                       m_linearTextureIndices, m_texCoordIndices, m_errors);
                *bindings[index]                  = binding.texture;
                *coordinates[index]               = binding.texCoord;
                material.textureTransforms[index] = binding.transform;
            }
            LoadMaterialFeatures(mat, material, pScene->textures, m_textureIndices, m_linearTextureIndices, m_texCoordIndices,
                                 m_errors);
            LoadAdditionalMaterialFeatures(sourceMaterials[materialIndex], material, pScene->textures, m_textureIndices,
                                           m_linearTextureIndices, m_texCoordIndices, m_errors);
        }
    }
}

const fastgltf::Accessor* GltfAssetReader::GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                             const char*                name,
                                                             fastgltf::AccessorType     type,
                                                             size_t                     vertexCount) const
{
    const fastgltf::Accessor* result     = nullptr;

    const fastgltf::Attribute* attribute = primitive.findAttribute(name);

    if (attribute != primitive.attributes.end())
    {
        const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute->accessorIndex];

        const std::string_view semantic(name);

        const bool integer  = accessor.componentType == fastgltf::ComponentType::UnsignedByte
                           || accessor.componentType == fastgltf::ComponentType::UnsignedShort;

        bool componentValid = accessor.componentType == fastgltf::ComponentType::Float
                           || accessor.componentType == fastgltf::ComponentType::Double;

        if (semantic.starts_with("JOINTS_"))
        {
            componentValid = integer && !accessor.normalized;
        }
        else if (semantic.starts_with("COLOR_") || semantic.starts_with("TEXCOORD_") || semantic.starts_with("WEIGHTS_"))
        {
            componentValid |= integer && accessor.normalized;
        }

        bool quantized = false;

        for (const FASTGLTF_STD_PMR_NS::string& extension : m_gltfAsset.extensionsUsed)
        {
            quantized |= extension == "KHR_mesh_quantization";
        }

        const bool signedInteger =
            accessor.componentType == fastgltf::ComponentType::Byte || accessor.componentType == fastgltf::ComponentType::Short;

        if (quantized)
        {
            if (semantic == "POSITION" || semantic.starts_with("TEXCOORD_"))
            {
                componentValid |= integer || signedInteger;
            }
            else if (semantic == "NORMAL" || semantic == "TANGENT")
            {
                componentValid |= signedInteger && accessor.normalized;
            }
        }

        const bool typeValid =
            accessor.type == type || (semantic.starts_with("COLOR_") && accessor.type == fastgltf::AccessorType::Vec3);

        if (componentValid && typeValid && (vertexCount == 0 || accessor.count == vertexCount))
        {
            result = &accessor;
        }
        else
        {
        }
    }

    return result;
}

template <typename T>
static T DecodeAttribute(const fastgltf::Asset& asset, const fastgltf::Accessor* accessor, size_t index, const T& fallback)
{
    T result = fallback;

    if (accessor != nullptr)
    {
        result = fastgltf::getAccessorElement<T>(asset, *accessor, index);
    }

    return result;
}


bool GltfAssetReader::LoadPrimitive(const fastgltf::Primitive& primitive,
                                    std::vector<Vertex>&       vertices,
                                    std::vector<uint32_t>&     indices,
                                    std::vector<uint32_t>&     sourceVertices,
                                    bool&                      flatNormals)
{
    vertices.clear();

    indices.clear();

    sourceVertices.clear();

    flatNormals                         = false;

    const fastgltf::Accessor* positions = GetVertexAccessor(primitive, "POSITION", fastgltf::AccessorType::Vec3, 0);

    bool valid                          = positions != nullptr;

    const bool triangles                = primitive.type == fastgltf::PrimitiveType::Triangles
                                       || primitive.type == fastgltf::PrimitiveType::TriangleStrip
                                       || primitive.type == fastgltf::PrimitiveType::TriangleFan;

    if (valid)
    {
        const size_t count = positions->count;

        if (primitive.materialIndex)
        {
            const fastgltf::Material& material = m_gltfAsset.materials[*primitive.materialIndex];

            // Unlit materials consume only base color; retained lighting-extension maps do
            // not impose coordinate requirements on a primitive that cannot sample them.
            const std::vector<TextureRole> roles =
                material.unlit
                    ? std::vector<TextureRole>{{material.pbrData.baseColorTexture ? &*material.pbrData.baseColorTexture
                                                                                  : nullptr,
                                                true}}
                    : GetMaterialTextureRoles(material);

            for (const TextureRole& role : roles)
            {
                if (role.first != nullptr)
                {
                    const fastgltf::TextureInfo& binding = *role.first;

                    const size_t coordinateSet           = binding.transform && binding.transform->texCoordIndex
                                                             ? *binding.transform->texCoordIndex
                                                             : binding.texCoordIndex;

                    const std::string semantic           = "TEXCOORD_" + std::to_string(coordinateSet);

                    if (GetVertexAccessor(primitive, semantic.c_str(), fastgltf::AccessorType::Vec2, count) == nullptr)
                    {
                        m_errors.Fail(
                            (std::string("glTF primitive material requires missing or invalid ") + std::string(semantic)));

                        valid = false;
                    }
                }
            }
        }

        const fastgltf::Accessor* normals  = GetVertexAccessor(primitive, "NORMAL", fastgltf::AccessorType::Vec3, count);

        const fastgltf::Accessor* tangents = GetVertexAccessor(primitive, "TANGENT", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* colors   = GetVertexAccessor(primitive, "COLOR_0", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* uv0      = GetVertexAccessor(primitive, "TEXCOORD_0", fastgltf::AccessorType::Vec2, count);

        const fastgltf::Accessor* uv1      = GetVertexAccessor(primitive, "TEXCOORD_1", fastgltf::AccessorType::Vec2, count);

        const fastgltf::Accessor* joints   = GetVertexAccessor(primitive, "JOINTS_0", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* weights  = GetVertexAccessor(primitive, "WEIGHTS_0", fastgltf::AccessorType::Vec4, count);

        bool generateNormals               = normals == nullptr;

        valid =
            valid && ResizeGeometryVector(vertices, count, m_errors) && ResizeGeometryVector(sourceVertices, count, m_errors);

        for (size_t index = 0; valid && !m_errors.Failed() && index < count; ++index)
        {
            Vertex& vertex         = vertices[index];

            sourceVertices[index]  = static_cast<uint32_t>(index);

            vertex.pos             = Vec4(DecodeAttribute(m_gltfAsset, positions, index, Vec3(0)), 1);

            valid                 &= std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z);

            Vec3 normal            = DecodeAttribute(m_gltfAsset, normals, index, Vec3(0));

            generateNormals       |= !NormalizeSurfaceNormal(normal);

            vertex.normal          = Vec4(normal, 0);

            vertex.tangent         = DecodeAttribute(m_gltfAsset, tangents, index, Vec4(0));

            vertex.uv0             = DecodeAttribute(m_gltfAsset, uv0, index, Vec2(0));

            vertex.uv1             = DecodeAttribute(m_gltfAsset, uv1, index, Vec2(0));

            vertex.joint0          = DecodeAttribute(m_gltfAsset, joints, index, Vec4(0));

            vertex.weight0         = DecodeAttribute(m_gltfAsset, weights, index, Vec4(0));

            vertex.color           = colors != nullptr && colors->type == fastgltf::AccessorType::Vec3
                                       ? Vec4(DecodeAttribute(m_gltfAsset, colors, index, Vec3(1)), 1)
                                       : DecodeAttribute(m_gltfAsset, colors, index, Vec4(1));
        }

        if (primitive.indicesAccessor.has_value())
        {
            const fastgltf::Accessor& accessor  = m_gltfAsset.accessors[*primitive.indicesAccessor];

            valid                              &= accessor.type == fastgltf::AccessorType::Scalar && !accessor.normalized
                                               && (accessor.componentType == fastgltf::ComponentType::UnsignedByte
                                                   || accessor.componentType == fastgltf::ComponentType::UnsignedShort
                                                   || accessor.componentType == fastgltf::ComponentType::UnsignedInt);

            if (valid)
            {
                valid = ResizeGeometryVector(indices, accessor.count, m_errors);
                if (valid)
                {
                    fastgltf::copyFromAccessor<uint32_t>(m_gltfAsset, accessor, indices.data());
                }
            }
        }
        else
        {
            valid = valid && ResizeGeometryVector(indices, count, m_errors);

            for (uint32_t index = 0; valid && !m_errors.Failed() && index < count; ++index)
            {
                indices[index] = index;
            }
        }

        if (valid && triangles && primitive.type != fastgltf::PrimitiveType::Triangles && indices.size() >= 3)
        {
            std::vector<uint32_t> triangles;

            valid = indices.size() - 2 <= SIZE_MAX / 3 && ReserveGeometryVector(triangles, (indices.size() - 2) * 3, m_errors);
            if (!valid && !m_errors.Failed())
            {
                m_errors.Fail("glTF triangle expansion exceeds address space", SceneImportStatus::ResourceLimit);
            }

            for (size_t index = 2; valid && !m_errors.Failed() && index < indices.size(); ++index)
            {
                const uint32_t a =
                    primitive.type == fastgltf::PrimitiveType::TriangleFan ? indices[0] : indices[index - 2 + (index & 1)];

                const uint32_t b = primitive.type == fastgltf::PrimitiveType::TriangleFan ? indices[index - 1]
                                                                                          : indices[index - 1 - (index & 1)];

                triangles.push_back(a);

                triangles.push_back(b);

                triangles.push_back(indices[index]);
            }

            indices = std::move(triangles);
        }

        if (valid
            && (primitive.type == fastgltf::PrimitiveType::LineStrip || primitive.type == fastgltf::PrimitiveType::LineLoop)
            && indices.size() >= 2)
        {
            std::vector<uint32_t> lines;
            valid = indices.size() <= SIZE_MAX / 2 && ReserveGeometryVector(lines, indices.size() * 2, m_errors);
            if (!valid && !m_errors.Failed())
            {
                m_errors.Fail("glTF line expansion exceeds address space", SceneImportStatus::ResourceLimit);
            }

            for (size_t index = 1; valid && !m_errors.Failed() && index < indices.size(); ++index)
            {
                lines.push_back(indices[index - 1]);

                lines.push_back(indices[index]);
            }

            if (valid && !m_errors.Failed() && primitive.type == fastgltf::PrimitiveType::LineLoop)
            {
                lines.push_back(indices[indices.size() - 1]);

                lines.push_back(indices[0]);
            }

            indices = std::move(lines);
        }

        valid &= !m_errors.Failed() && !indices.empty()
              && (triangles ? indices.size() % 3 == 0
                            : primitive.type == fastgltf::PrimitiveType::Points || indices.size() % 2 == 0);

        for (uint32_t index : indices)
        {
            valid &= index < count;
        }

        if (valid && triangles && generateNormals)
        {
            // Existing source arrays remain live while flat corner arrays are built.
            valid = m_errors.ReserveGeometryBytes(indices.size(), sizeof(Vertex) + sizeof(uint32_t));
            if (valid)
            {
                GenerateFlatNormals(vertices, indices, sourceVertices);
                flatNormals = true;
            }
        }
    }

    if (!valid)
    {
    }

    return valid;
}

struct GltfAssetReader::PrimitiveData
{
    std::vector<std::vector<VertexJointInfluence>> influences;
    std::vector<Vertex>                            vertices;
    std::vector<uint32_t>                          indices;
    std::vector<uint32_t>                          sourceVertices;
    std::vector<VertexTextureCoordinates>          texCoords;
    Bounds                                         bounds;
    bool                                           flatNormals{false};
    bool                                           valid{false};
};

GltfAssetReader::PrimitiveData GltfAssetReader::ProcessPrimitive(const fastgltf::Primitive& primitive)
{
    PrimitiveData result;
    if (!m_errors.Failed())
    {
        result.valid = LoadPrimitive(primitive, result.vertices, result.indices, result.sourceVertices, result.flatNormals);
    }
    if (result.valid)
    {
        std::vector<std::pair<uint32_t, const fastgltf::Accessor*>> texCoords;
        const uint32_t texCoordCount = static_cast<uint32_t>(m_texCoordIndices.size());
        for (const fastgltf::Attribute& attribute : primitive.attributes)
        {
            if (attribute.name.starts_with("TEXCOORD_"))
            {
                size_t                       sourceSet = 0;
                const std::string_view       semantic(attribute.name);
                const std::from_chars_result parsed =
                    std::from_chars(semantic.data() + 9, semantic.data() + semantic.size(), sourceSet);
                const std::map<size_t, uint32_t>::const_iterator found = m_texCoordIndices.find(sourceSet);
                if (parsed.ec != std::errc{} || parsed.ptr != semantic.data() + semantic.size()
                    || found == m_texCoordIndices.end())
                {
                    m_errors.Fail("glTF primitive has an invalid texture coordinate set");
                    result.valid = false;
                }
                else
                {
                    const fastgltf::Accessor* accessor =
                        GetVertexAccessor(primitive, attribute.name.c_str(), fastgltf::AccessorType::Vec2, 0);
                    if (accessor != nullptr)
                    {
                        if (ReserveGeometryVector(texCoords, texCoords.size() + 1, m_errors))
                        {
                            texCoords.push_back({found->second, accessor});
                        }
                    }
                }
            }
        }
        result.valid = result.valid && ReserveGeometryVector(result.texCoords, result.vertices.size(), m_errors);
        for (size_t index = 0; index < result.vertices.size() && result.valid; ++index)
        {
            result.bounds.SetMin(Vec3(result.vertices[index].pos));
            result.bounds.SetMax(Vec3(result.vertices[index].pos));
            VertexTextureCoordinates coordinates;
            result.valid = ResizeGeometryVector(coordinates, texCoordCount, m_errors);
            for (Vec2& coordinate : coordinates)
            {
                coordinate = Vec2(0);
            }
            for (const std::pair<uint32_t, const fastgltf::Accessor*>& coordinate : texCoords)
            {
                if (result.valid)
                {
                    coordinates[coordinate.first] =
                        fastgltf::getAccessorElement<Vec2>(m_gltfAsset, *coordinate.second, result.sourceVertices[index]);
                }
            }
            if (result.valid)
            {
                result.texCoords.push_back(std::move(coordinates));
            }
        }
    }
    if (result.valid)
    {
        for (const fastgltf::Attribute& attribute : primitive.attributes)
        {
            if (attribute.name.starts_with("JOINTS_"))
            {
                const std::string         weightsName = "WEIGHTS_" + std::string(attribute.name.substr(7));
                const fastgltf::Accessor* joints =
                    GetVertexAccessor(primitive, attribute.name.c_str(), fastgltf::AccessorType::Vec4, 0);
                const fastgltf::Accessor* weights = GetVertexAccessor(
                    primitive, weightsName.c_str(), fastgltf::AccessorType::Vec4, joints != nullptr ? joints->count : 0);
                if (joints != nullptr && weights != nullptr)
                {
                    result.valid = result.valid && ResizeGeometryVector(result.influences, result.vertices.size(), m_errors);
                    for (size_t vertex = 0; result.valid && !m_errors.Failed() && vertex < result.vertices.size(); ++vertex)
                    {
                        const fastgltf::math::u32vec4 joint = fastgltf::getAccessorElement<fastgltf::math::u32vec4>(
                            m_gltfAsset, *joints, result.sourceVertices[vertex]);
                        const Vec4 weight =
                            fastgltf::getAccessorElement<Vec4>(m_gltfAsset, *weights, result.sourceVertices[vertex]);
                        for (uint32_t component = 0; component < 4; ++component)
                        {
                            if (result.valid && weight[component] > 0)
                            {
                                result.valid = ReserveGeometryVector(result.influences[vertex],
                                                                     result.influences[vertex].size() + 1, m_errors);
                                if (result.valid)
                                {
                                    result.influences[vertex].push_back({joint[component], weight[component]});
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

void GltfAssetReader::LoadGltfMeshes(Scene* pScene)
{
    ZENASSET_MEASURE_SCOPE(SourceMeshes);
    std::vector<const fastgltf::Primitive*> primitives;
    for (const fastgltf::Mesh& mesh : m_gltfAsset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            if (ReserveGeometryVector(primitives, primitives.size() + 1, m_errors))
            {
                primitives.push_back(&primitive);
            }
        }
    }
    const size_t                            window = std::min(primitives.size(), std::max(size_t(1), m_workers->GetSize() * 2));
    std::vector<std::future<PrimitiveData>> pending;
    if (ResizeGeometryVector(pending, window, m_errors)
        && ReserveGeometryVector(pScene->meshes, m_gltfAsset.meshes.size(), m_errors))
    {
        for (size_t index = 0; index < window; ++index)
        {
            pending[index] = m_workers->Push(&GltfAssetReader::ProcessPrimitive, this, std::cref(*primitives[index]));
        }
        size_t completed = 0;
        for (const fastgltf::Mesh& source : m_gltfAsset.meshes)
        {
            MeshAsset mesh;
            if (ReserveGeometryVector(mesh.primitives, source.primitives.size(), m_errors)
                && ReserveGeometryVector(mesh.morphWeights, source.weights.size(), m_errors))
            {
                mesh.name = std::string(source.name);
                for (float weight : source.weights)
                {
                    mesh.morphWeights.push_back(weight);
                }
            }
            for (const fastgltf::Primitive& primitive : source.primitives)
            {
                // Drain every dispatched result even after admission/cancellation
                // failure. Later tasks see the shared failure before allocating.
                const size_t  slot      = completed % window;
                PrimitiveData processed = pending[slot].get();
                const size_t  next      = completed + window;
                if (next < primitives.size())
                {
                    pending[slot] = m_workers->Push(&GltfAssetReader::ProcessPrimitive, this, std::cref(*primitives[next]));
                }
                ++completed;
                if (!m_errors.Failed())
                {
                    MeshPrimitiveAsset output;
                    output.name = "Mesh_" + std::string(source.name) + "_SubMesh#" + std::to_string(mesh.primitives.size());
                    output.firstVertex = static_cast<uint32_t>(pScene->vertices.size());
                    output.firstIndex  = static_cast<uint32_t>(pScene->indices.size());
                    output.material =
                        MaterialId{primitive.materialIndex ? static_cast<uint32_t>(*primitive.materialIndex) : UINT32_MAX};
                    output.topology = primitive.type == fastgltf::PrimitiveType::Points ? MeshTopology::Points
                                    : primitive.type == fastgltf::PrimitiveType::Lines
                                              || primitive.type == fastgltf::PrimitiveType::LineLoop
                                              || primitive.type == fastgltf::PrimitiveType::LineStrip
                                        ? MeshTopology::Lines
                                        : MeshTopology::Triangles;
                    if (processed.valid)
                    {
                        const bool countsValid = processed.vertices.size() <= UINT32_MAX - pScene->vertices.size()
                                              && processed.indices.size() <= UINT32_MAX - pScene->indices.size();
                        if (!countsValid)
                        {
                            m_errors.Fail("glTF merged geometry exceeds 32-bit counts", SceneImportStatus::ResourceLimit);
                        }
                        else if (ReserveGeometryVector(pScene->vertices, pScene->vertices.size() + processed.vertices.size(),
                                                       m_errors)
                                 && ReserveGeometryVector(pScene->vertexTexCoords,
                                                          pScene->vertexTexCoords.size() + processed.texCoords.size(), m_errors)
                                 && ReserveGeometryVector(pScene->indices, pScene->indices.size() + processed.indices.size(),
                                                          m_errors))
                        {
                            output.vertexCount    = static_cast<uint32_t>(processed.vertices.size());
                            output.indexCount     = static_cast<uint32_t>(processed.indices.size());
                            output.bounds         = processed.bounds;
                            output.flatNormals    = processed.flatNormals;
                            output.sourceVertices = std::move(processed.sourceVertices);
                            output.influences     = std::move(processed.influences);
                            // Result storage remains charged while destination storage
                            // grows. Moved nested UV/influence buffers are charged once.
                            pScene->vertices.insert(pScene->vertices.end(), processed.vertices.begin(),
                                                    processed.vertices.end());
                            for (VertexTextureCoordinates& coordinates : processed.texCoords)
                            {
                                pScene->vertexTexCoords.push_back(std::move(coordinates));
                            }
                            for (uint32_t index : processed.indices)
                            {
                                pScene->indices.push_back(output.firstVertex + index);
                            }
                        }
                    }
                    if (!m_errors.Failed())
                    {
                        mesh.primitives.push_back(std::move(output));
                    }
                }
            }
            if (!m_errors.Failed())
            {
                pScene->meshes.push_back(std::move(mesh));
            }
        }
    }
}

static std::vector<float> DecodeAnimationValues(const fastgltf::Asset&    asset,
                                                const fastgltf::Accessor& accessor,
                                                ImportErrors&             errors)
{
    const uint32_t components = fastgltf::getNumComponents(accessor.type);

    std::vector<float> values;
    if (components == 0 || accessor.count > SIZE_MAX / components)
    {
        errors.Fail("glTF animation values exceed address space", SceneImportStatus::ResourceLimit);
    }
    else
    {
        ResizeGeometryVector(values, accessor.count * components, errors);
    }

    for (size_t index = 0; index < accessor.count && !errors.Failed(); ++index)
    {
        Vec4 value(0.0f);

        switch (accessor.type)
        {
            case fastgltf::AccessorType::Scalar: value.x = fastgltf::getAccessorElement<float>(asset, accessor, index); break;

            case fastgltf::AccessorType::Vec2:
                value = Vec4(fastgltf::getAccessorElement<Vec2>(asset, accessor, index), 0.0f, 0.0f);

                break;

            case fastgltf::AccessorType::Vec3:
                value = Vec4(fastgltf::getAccessorElement<Vec3>(asset, accessor, index), 0.0f);

                break;

            case fastgltf::AccessorType::Vec4: value = fastgltf::getAccessorElement<Vec4>(asset, accessor, index); break;

            default: errors.Fail("Invalid glTF animation output type"); break;
        }

        for (uint32_t component = 0; component < components; ++component)
        {
            values[index * components + component] = value[component];
        }
    }

    return values;
}

static float ImageLightLinearChannel(float channel)
{
    const float result = channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);

    return result;
}

// An invalid face records an error and returns no radiance.
static std::vector<Vec4> DecodeImageLightFace(const fastgltf::Asset&  asset,
                                              uint32_t                imageIndex,
                                              uint32_t                size,
                                              ImportErrors&           errors,
                                              const GltfFileProgress& progress)
{
    ZENASSET_MEASURE_SCOPE(SourceImages);
    std::vector<Vec4> radiance;

    std::vector<std::byte> fileBytes;

    fastgltf::span<const std::byte> bytes;

    std::vector<uint8_t> pixels;

    uint32_t width  = 0;

    uint32_t height = 0;

    if (imageIndex >= asset.images.size())
    {
        errors.Fail("Image based light references an invalid image");
    }
    else
    {
        bytes  = GetImageBytes(asset, asset.images[imageIndex], fileBytes, errors);

        pixels = DecodeImagePixels(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), width, height, errors);

        if (errors.Failed() || width != size || height != size || pixels.empty())
        {
            errors.Fail("Image based light cubemap face has invalid dimensions or pixels");
        }
    }

    const uint64_t radianceCount = uint64_t(size) * size;
    if (!errors.Failed() && radianceCount > SIZE_MAX / sizeof(Vec4))
    {
        errors.Fail("Image based light radiance exceeds the allocation limit", SceneImportStatus::ResourceLimit);
    }
    if (!errors.Failed() && errors.ReserveImageBytes(radianceCount * sizeof(Vec4)))
    {
        int sourceWidth  = 0;

        int sourceHeight = 0;

        int channels     = 0;

        const uint8_t pngIdentifier[]{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

        const bool png =
            bytes.size() >= sizeof(pngIdentifier) && std::memcmp(bytes.data(), pngIdentifier, sizeof(pngIdentifier)) == 0;

        if (bytes.size() <= INT_MAX)
        {
            stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &sourceWidth,
                                  &sourceHeight, &channels);
        }

        const bool rgbd = png && channels == 4;

        radiance.resize(size_t(size) * size);

        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                // EXT_lights_image_based images are flipped about their vertical axis.
                const size_t source = (size_t(y) * size + size - 1 - x) * 4;

                const Vec3 color(float(pixels[source]) / 255.0f, float(pixels[source + 1]) / 255.0f,
                                 float(pixels[source + 2]) / 255.0f);

                const Vec3 linear = rgbd ? Pow(color, Vec3(2.2f)) / std::max(float(pixels[source + 3]) / 255.0f, 1.0f / 255.0f)
                                         : Vec3(ImageLightLinearChannel(color.x), ImageLightLinearChannel(color.y),
                                                ImageLightLinearChannel(color.z));

                radiance[size_t(y) * size + x] = Vec4(linear, 1.0f);
            }
        }
    }

    if (!errors.Failed() && !radiance.empty())
    {
        progress.CompleteImage(imageIndex);
    }
    return radiance;
}

static void ReadImageLightIrradiance(const simdjson::dom::element& source, ImageBasedLightAsset& light, ImportErrors& errors)
{
    simdjson::dom::array coefficients;

    if (source["irradianceCoefficients"].get_array().get(coefficients) != simdjson::SUCCESS || coefficients.size() != 9)
    {
        errors.Fail("Image based light requires nine irradiance coefficients");
    }
    else
    {
        for (simdjson::dom::element coefficient : coefficients)
        {
            simdjson::dom::array values;

            if (coefficient.get_array().get(values) != simdjson::SUCCESS || values.size() != 3)
            {
                errors.Fail("Image based light irradiance coefficient requires three channels");
            }
            else
            {
                Vec3 value(0.0f);

                uint32_t channel = 0;

                for (simdjson::dom::element entry : values)
                {
                    double number = 0.0;

                    if (entry.get_double().get(number) != simdjson::SUCCESS || !std::isfinite(number))
                    {
                        errors.Fail("Invalid image based light irradiance coefficient");
                    }
                    else
                    {
                        value[channel] = static_cast<float>(number);
                    }

                    ++channel;
                }

                light.irradianceCoefficients.push_back(value);
            }
        }
    }
}

static void ReadImageLightMip(const simdjson::dom::element& mip,
                              const fastgltf::Asset&        asset,
                              EnvironmentCubePixels&        cube,
                              ImportErrors&                 errors,
                              const GltfFileProgress&       progress)
{
    simdjson::dom::array faces;

    if (mip.get_array().get(faces) != simdjson::SUCCESS || faces.size() != 6 || cube.mipLevels >= 32)
    {
        errors.Fail("Image based light requires six faces per specular mip");
    }
    else
    {
        const uint32_t mipSize = std::max(1u, cube.size >> cube.mipLevels);

        for (simdjson::dom::element face : faces)
        {
            uint64_t imageIndex = 0;

            if (face.get_uint64().get(imageIndex) != simdjson::SUCCESS || imageIndex >= asset.images.size())
            {
                errors.Fail("Invalid image based light specular image index");
            }
            else if (!errors.Failed())
            {
                cube.rgba32FloatMipFaces.push_back(
                    DecodeImageLightFace(asset, static_cast<uint32_t>(imageIndex), mipSize, errors, progress));
            }
        }

        ++cube.mipLevels;
    }
}

static void ReadImageLightSpecular(const simdjson::dom::element& source,
                                   const fastgltf::Asset&        asset,
                                   ImageBasedLightAsset&         light,
                                   ImportErrors&                 errors,
                                   const GltfFileProgress&       progress)
{
    uint64_t size = 0;

    simdjson::dom::array mips;

    if (source["specularImageSize"].get_uint64().get(size) != simdjson::SUCCESS || size == 0 || size > UINT32_MAX
        || (size & (size - 1)) != 0)
    {
        errors.Fail("Image based light requires a power of two cubemap size");
    }
    else if (source["specularImages"].get_array().get(mips) != simdjson::SUCCESS || mips.size() == 0)
    {
        errors.Fail("Image based light requires specular cubemap images");
    }
    else
    {
        std::shared_ptr<EnvironmentCubePixels> cube = std::make_shared<EnvironmentCubePixels>();
        cube->size                                  = static_cast<uint32_t>(size);

        for (simdjson::dom::element mip : mips)
        {
            if (!errors.Failed())
            {
                ReadImageLightMip(mip, asset, *cube, errors, progress);
            }
        }
        if (!errors.Failed())
        {
            light.specular = std::move(cube);
        }
    }
}

static void LoadImageBasedLights(const simdjson::dom::element& document,
                                 const fastgltf::Asset&        asset,
                                 Scene&                        data,
                                 ImportErrors&                 errors,
                                 const GltfFileProgress&       progress)
{
    simdjson::dom::array lights;

    if (document.at_pointer("/extensions/EXT_lights_image_based/lights").get_array().get(lights) == simdjson::SUCCESS)
    {
        for (simdjson::dom::element source : lights)
        {
            if (!errors.Failed())
            {
                ImageBasedLightAsset light;

                std::string_view name;

                if (source["name"].get_string().get(name) == simdjson::SUCCESS)
                {
                    light.name = std::string(name);
                }

                double intensity = 1.0;

                source["intensity"].get_double().get(intensity);

                light.intensity = static_cast<float>(intensity);

                float rotation[]{0.0f, 0.0f, 0.0f, 1.0f};

                ReadJsonVector(source, "rotation", rotation, 4, errors);

                light.rotation = Quat(rotation[3], rotation[0], rotation[1], rotation[2]);

                ReadImageLightIrradiance(source, light, errors);

                if (!errors.Failed())
                {
                    ReadImageLightSpecular(source, asset, light, errors, progress);
                }

                data.imageBasedLights.push_back(std::move(light));
            }
        }

        uint64_t selected = 0;

        const std::string pointer =
            "/scenes/" + std::to_string(asset.defaultScene.value_or(0)) + "/extensions/EXT_lights_image_based/light";

        if (!errors.Failed() && document.at_pointer(pointer).get_uint64().get(selected) == simdjson::SUCCESS)
        {
            if (selected >= data.imageBasedLights.size())
            {
                errors.Fail("Scene references an invalid image based light");
            }
            else
            {
                data.imageBasedLight = static_cast<int32_t>(selected);
            }
        }
    }
}

template <typename T> static void CopyRetainedAccessorValues(const fastgltf::Asset&    asset,
                                                             const fastgltf::Accessor& accessor,
                                                             VertexAttributeAsset&     retained)
{
    for (size_t index = 0; index < accessor.count; ++index)
    {
        const T value = fastgltf::getAccessorElement<T>(asset, accessor, index);

        if constexpr (std::is_arithmetic_v<T>)
        {
            retained.values[index] = value;
        }
        else
        {
            for (uint32_t component = 0; component < retained.components; ++component)
            {
                retained.values[index * retained.components + component] = value[component];
            }
        }
    }
}

static VertexAttributeAsset DecodeRetainedVertexAttribute(const fastgltf::Asset&     asset,
                                                          const fastgltf::Attribute& attribute,
                                                          uint32_t                   mesh,
                                                          uint32_t                   primitive,
                                                          ImportErrors&              errors,
                                                          int32_t                    morphTarget = -1)
{
    const fastgltf::Accessor& accessor = asset.accessors[attribute.accessorIndex];

    VertexAttributeAsset retained;

    retained.mesh          = mesh;

    retained.primitive     = primitive;

    retained.semantic      = std::string(attribute.name);

    retained.morphTarget   = morphTarget;

    retained.components    = static_cast<uint32_t>(fastgltf::getNumComponents(accessor.type));

    retained.componentType = fastgltf::getGLComponentType(accessor.componentType);

    retained.normalized    = accessor.normalized;

    if (retained.components == 0 || accessor.count > SIZE_MAX / retained.components)
    {
        errors.Fail("glTF custom attribute values exceed address space", SceneImportStatus::ResourceLimit);
    }
    else
    {
        ResizeGeometryVector(retained.values, accessor.count * retained.components, errors);
    }

    if (!errors.Failed())
    {
        switch (accessor.type)
        {
            case fastgltf::AccessorType::Scalar: CopyRetainedAccessorValues<double>(asset, accessor, retained); break;
            case fastgltf::AccessorType::Vec2:
                CopyRetainedAccessorValues<fastgltf::math::dvec2>(asset, accessor, retained);
                break;
            case fastgltf::AccessorType::Vec3:
                CopyRetainedAccessorValues<fastgltf::math::dvec3>(asset, accessor, retained);
                break;
            case fastgltf::AccessorType::Vec4:
                CopyRetainedAccessorValues<fastgltf::math::dvec4>(asset, accessor, retained);
                break;
            default: errors.Fail("Invalid glTF vertex attribute type"); break;
        }
    }

    return retained;
}

void GltfAssetReader::LoadGltfAssetData(Scene* pScene)
{
    Scene& data         = *pScene;

    data.sourceDocument = m_sourceJson;

    simdjson::dom::parser jsonParser;

    simdjson::dom::element document;

    if (jsonParser.parse(m_json).get(document) != simdjson::SUCCESS)
    {
        m_errors.Fail("Failed to parse glTF extension metadata");
    }
    else
    {
        LoadImageBasedLights(document, m_gltfAsset, data, m_errors, m_fileProgress);
    }

    for (const std::string& name : m_gltfAsset.materialVariants)
    {
        data.materialVariants.push_back(name);
    }

    for (const fastgltf::Skin& source : m_gltfAsset.skins)
    {
        SkinAsset skin;

        skin.name     = std::string(source.name);

        skin.skeleton = source.skeleton ? static_cast<int32_t>(*source.skeleton) : -1;

        for (size_t joint : source.joints)
        {
            if (ReserveGeometryVector(skin.joints, skin.joints.size() + 1, m_errors))
            {
                skin.joints.push_back(static_cast<uint32_t>(joint));
            }
        }

        if (ResizeGeometryVector(skin.inverseBindMatrices, skin.joints.size(), m_errors))
        {
            std::fill(skin.inverseBindMatrices.begin(), skin.inverseBindMatrices.end(), Mat4(1.0f));
        }

        if (source.inverseBindMatrices && !m_errors.Failed())
        {
            const fastgltf::Accessor& accessor = m_gltfAsset.accessors[*source.inverseBindMatrices];

            if (accessor.count < skin.joints.size())
            {
                m_errors.Fail("Too few glTF inverse bind matrices");
            }
            else
            {
                for (size_t joint = 0; joint < skin.joints.size(); ++joint)
                {
                    skin.inverseBindMatrices[joint] = fastgltf::getAccessorElement<Mat4>(m_gltfAsset, accessor, joint);
                }
            }
        }

        if (ReserveGeometryVector(data.skins, data.skins.size() + 1, m_errors))
        {
            data.skins.push_back(std::move(skin));
        }
    }

    uint32_t animationIndex = 0;

    for (const fastgltf::Animation& source : m_gltfAsset.animations)
    {
        AnimationAsset animation;

        animation.name = std::string(source.name);

        for (const fastgltf::AnimationSampler& sourceSampler : source.samplers)
        {
            AnimationSampler sampler;

            sampler.interpolation            = static_cast<AnimationInterpolation>(sourceSampler.interpolation);

            const fastgltf::Accessor& input  = m_gltfAsset.accessors[sourceSampler.inputAccessor];

            const fastgltf::Accessor& output = m_gltfAsset.accessors[sourceSampler.outputAccessor];

            sampler.components               = fastgltf::getNumComponents(output.type);

            if (ResizeGeometryVector(sampler.times, input.count, m_errors))
            {
                fastgltf::copyFromAccessor<float>(m_gltfAsset, input, sampler.times.data());
            }

            sampler.values = DecodeAnimationValues(m_gltfAsset, output, m_errors);

            if (ReserveGeometryVector(animation.samplers, animation.samplers.size() + 1, m_errors))
            {
                animation.samplers.push_back(std::move(sampler));
            }
        }

        uint32_t channelIndex = 0;

        for (const fastgltf::AnimationChannel& sourceChannel : source.channels)
        {
            const std::string pointerPath = "/animations/" + std::to_string(animationIndex) + "/channels/"
                                          + std::to_string(channelIndex) + "/target/extensions/KHR_animation_pointer/pointer";

            std::string_view pointer;

            const bool hasPointer = document.at_pointer(pointerPath).get_string().get(pointer) == simdjson::SUCCESS;

            if (sourceChannel.nodeIndex)
            {
                AnimationChannel channel;

                channel.sampler = static_cast<uint32_t>(sourceChannel.samplerIndex);

                channel.node    = static_cast<uint32_t>(*sourceChannel.nodeIndex);

                channel.path    = static_cast<AnimationPath>(static_cast<uint32_t>(sourceChannel.path) - 1);

                if (ReserveGeometryVector(animation.channels, animation.channels.size() + 1, m_errors))
                {
                    animation.channels.push_back(channel);
                }
            }
            else if (hasPointer)
            {
                AnimationChannel channel;

                channel.sampler      = static_cast<uint32_t>(sourceChannel.samplerIndex);

                channel.path         = AnimationPath::Property;

                channel.sourceTarget = std::string(pointer);

                if (ReserveGeometryVector(animation.channels, animation.channels.size() + 1, m_errors))
                {
                    animation.channels.push_back(std::move(channel));
                }
            }

            ++channelIndex;
        }

        if (ReserveGeometryVector(data.animations, data.animations.size() + 1, m_errors))
        {
            data.animations.push_back(std::move(animation));
        }

        ++animationIndex;
    }

    for (uint32_t meshIndex = 0; meshIndex < m_gltfAsset.meshes.size(); ++meshIndex)
    {
        const fastgltf::Mesh& mesh = m_gltfAsset.meshes[meshIndex];

        for (uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            const fastgltf::Primitive& primitive = mesh.primitives[primitiveIndex];

            for (const fastgltf::Attribute& attribute : primitive.attributes)
            {
                if (attribute.name != "POSITION" && attribute.name != "NORMAL" && attribute.name != "TANGENT"
                    && attribute.name != "COLOR_0" && !attribute.name.starts_with("TEXCOORD_"))
                {
                    if (ReserveGeometryVector(data.extraVertexAttributes, data.extraVertexAttributes.size() + 1, m_errors))
                    {
                        VertexAttributeAsset retained =
                            DecodeRetainedVertexAttribute(m_gltfAsset, attribute, meshIndex, primitiveIndex, m_errors);
                        if (!m_errors.Failed())
                        {
                            data.extraVertexAttributes.push_back(std::move(retained));
                        }
                    }
                }
            }

            if (!primitive.mappings.empty())
            {
                MaterialVariantPrimitiveAsset variant;

                variant.mesh      = meshIndex;

                variant.primitive = primitiveIndex;

                for (const fastgltf::Optional<size_t>& material : primitive.mappings)
                {
                    if (ReserveGeometryVector(variant.materials, variant.materials.size() + 1, m_errors))
                    {
                        variant.materials.push_back(material ? static_cast<int32_t>(*material) : -1);
                    }
                }

                if (ReserveGeometryVector(data.variantPrimitives, data.variantPrimitives.size() + 1, m_errors))
                {
                    data.variantPrimitives.push_back(std::move(variant));
                }
            }

            if (!primitive.targets.empty())
            {
                MorphPrimitiveAsset morph;

                morph.mesh      = meshIndex;

                morph.primitive = primitiveIndex;

                for (float weight : mesh.weights)
                {
                    if (ReserveGeometryVector(morph.weights, morph.weights.size() + 1, m_errors))
                    {
                        morph.weights.push_back(weight);
                    }
                }

                for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& attributes : primitive.targets)
                {
                    MorphTargetAsset target;

                    for (const fastgltf::Attribute& attribute : attributes)
                    {
                        const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute.accessorIndex];

                        std::vector<Vec3>* values          = attribute.name == "POSITION" ? &target.positions
                                                           : attribute.name == "NORMAL"   ? &target.normals
                                                           : attribute.name == "TANGENT"  ? &target.tangents
                                                                                          : nullptr;

                        if (values != nullptr)
                        {
                            if (ResizeGeometryVector(*values, accessor.count, m_errors))
                            {
                                fastgltf::copyFromAccessor<Vec3>(m_gltfAsset, accessor, values->data());
                            }
                        }
                        else if (attribute.name.starts_with("TEXCOORD_"))
                        {
                            const size_t sourceSet = static_cast<size_t>(std::stoull(std::string(attribute.name.substr(9))));

                            if (ResizeGeometryVector(target.texCoords, m_texCoordIndices.size(), m_errors))
                            {
                                std::vector<Vec2>& coordinates = target.texCoords[m_texCoordIndices.at(sourceSet)];
                                if (ResizeGeometryVector(coordinates, accessor.count, m_errors))
                                {
                                    fastgltf::copyFromAccessor<Vec2>(m_gltfAsset, accessor, coordinates.data());
                                }
                            }
                        }
                        else if (attribute.name.starts_with("COLOR_"))
                        {
                            MorphColorAttributeAsset color;

                            color.set = std::stoull(std::string(attribute.name.substr(6)));

                            if (ResizeGeometryVector(color.values, accessor.count, m_errors))
                            {
                                std::fill(color.values.begin(), color.values.end(), Vec4(0));
                            }

                            if (!m_errors.Failed() && accessor.type == fastgltf::AccessorType::Vec4)
                            {
                                fastgltf::copyFromAccessor<Vec4>(m_gltfAsset, accessor, color.values.data());
                            }
                            else if (!m_errors.Failed())
                            {
                                for (size_t index = 0; !m_errors.Failed() && index < accessor.count; ++index)
                                {
                                    color.values[index] =
                                        Vec4(fastgltf::getAccessorElement<Vec3>(m_gltfAsset, accessor, index), 0);
                                }
                            }

                            if (color.set == 0)
                            {
                                target.colors = std::move(color.values);
                            }
                            else
                            {
                                if (ReserveGeometryVector(target.extraColors, target.extraColors.size() + 1, m_errors))
                                {
                                    target.extraColors.push_back(std::move(color));
                                }
                            }
                        }
                        else
                        {
                            if (ReserveGeometryVector(data.extraVertexAttributes, data.extraVertexAttributes.size() + 1,
                                                      m_errors))
                            {
                                VertexAttributeAsset retained =
                                    DecodeRetainedVertexAttribute(m_gltfAsset, attribute, meshIndex, primitiveIndex, m_errors,
                                                                  static_cast<int32_t>(morph.targets.size()));
                                if (!m_errors.Failed())
                                {
                                    data.extraVertexAttributes.push_back(std::move(retained));
                                }
                            }
                        }
                    }

                    if (ReserveGeometryVector(morph.targets, morph.targets.size() + 1, m_errors))
                    {
                        morph.targets.push_back(std::move(target));
                    }
                }

                if (ReserveGeometryVector(data.morphPrimitives, data.morphPrimitives.size() + 1, m_errors))
                {
                    data.morphPrimitives.push_back(std::move(morph));
                }
            }
        }
    }
}

void GltfAssetReader::LoadGltfRenderableNodes(Scene* pScene)
{
    pScene->nodes.resize(m_gltfAsset.nodes.size());
    for (uint32_t index = 0; index < m_gltfAsset.nodes.size(); ++index)
    {
        const fastgltf::Node& source = m_gltfAsset.nodes[index];
        NodeAsset&            node   = pScene->nodes[index];
        node.name                    = std::string(source.name);
        node.visible                 = source.visible;
        node.selectable              = source.selectable;
        node.hoverable               = source.hoverable;
        node.mesh                    = MeshId{source.meshIndex ? static_cast<uint32_t>(*source.meshIndex) : UINT32_MAX};
        node.skin                    = SkinId{source.skinIndex ? static_cast<uint32_t>(*source.skinIndex) : UINT32_MAX};
        node.camera                  = CameraId{source.cameraIndex ? static_cast<uint32_t>(*source.cameraIndex) : UINT32_MAX};
        node.light                   = LightId{source.lightIndex ? static_cast<uint32_t>(*source.lightIndex) : UINT32_MAX};
        node.renderMesh              = source.instancingAttributes.empty();
        for (float weight : source.weights)
        {
            if (ReserveGeometryVector(node.morphWeights, node.morphWeights.size() + 1, m_errors))
            {
                node.morphWeights.push_back(weight);
            }
        }
        const fastgltf::TRS* trs = std::get_if<fastgltf::TRS>(&source.transform);
        if (trs != nullptr)
        {
            node.translation = MakeVec3(trs->translation.data());
            node.rotation    = MakeQuat(trs->rotation.data());
            node.scale       = MakeVec3(trs->scale.data());
        }
        else
        {
            node.matrixTransform = true;
            node.baseMatrix      = MakeMat4(std::get<fastgltf::math::fmat4x4>(source.transform).data());
        }
        for (size_t child : source.children)
        {
            node.children.push_back(NodeId{static_cast<uint32_t>(child)});
            pScene->nodes[child].parent = NodeId{index};
        }
    }
    if (!m_gltfAsset.scenes.empty())
    {
        for (size_t index : m_gltfAsset.scenes[m_gltfAsset.defaultScene.value_or(0)].nodeIndices)
        {
            pScene->roots.push_back(NodeId{static_cast<uint32_t>(index)});
        }
    }
    else
    {
        for (uint32_t index = 0; index < pScene->nodes.size(); ++index)
        {
            if (!pScene->nodes[index].parent.IsValid())
            {
                pScene->roots.push_back(NodeId{index});
            }
        }
    }
    for (uint32_t index = 0; index < m_gltfAsset.nodes.size(); ++index)
    {
        LoadGltfInstances(index, *pScene);
    }
    for (const fastgltf::Camera& source : m_gltfAsset.cameras)
    {
        CameraAsset camera;
        camera.name                                      = std::string(source.name);
        const fastgltf::Camera::Perspective* perspective = std::get_if<fastgltf::Camera::Perspective>(&source.camera);
        if (perspective != nullptr)
        {
            camera.verticalFov = perspective->yfov;
            camera.nearPlane   = perspective->znear;
            camera.infiniteFar = !perspective->zfar.has_value();
            camera.farPlane    = perspective->zfar.value_or(100.0f);
            camera.fixedAspect = perspective->aspectRatio.has_value();
            camera.aspect      = perspective->aspectRatio.value_or(1.0f);
        }
        else
        {
            const fastgltf::Camera::Orthographic& ortho = std::get<fastgltf::Camera::Orthographic>(source.camera);
            camera.orthographic                         = true;
            camera.xmag                                 = ortho.xmag;
            camera.ymag                                 = ortho.ymag;
            camera.nearPlane                            = ortho.znear;
            camera.farPlane                             = ortho.zfar;
        }
        pScene->cameras.push_back(std::move(camera));
    }
    for (const fastgltf::Light& source : m_gltfAsset.lights)
    {
        LightAsset light;
        light.name           = std::string(source.name);
        light.kind           = source.type == fastgltf::LightType::Directional ? LightKind::Directional
                             : source.type == fastgltf::LightType::Spot        ? LightKind::Spot
                                                                               : LightKind::Point;
        light.color          = MakeVec3(source.color.data());
        light.intensity      = source.intensity;
        light.range          = source.range.value_or(0.0f);
        light.innerConeAngle = source.innerConeAngle.value_or(0.0f);
        light.outerConeAngle = source.outerConeAngle.value_or(QuarterPi);
        pScene->lights.push_back(std::move(light));
    }
}

void GltfAssetReader::LoadGltfInstances(uint32_t nodeIndex, Scene& scene)
{
    const fastgltf::Node& source = m_gltfAsset.nodes[nodeIndex];
    if (source.meshIndex && !source.instancingAttributes.empty())
    {
        size_t                    instanceCount = 0;
        const fastgltf::Accessor* translations  = nullptr;
        const fastgltf::Accessor* rotations     = nullptr;
        const fastgltf::Accessor* scales        = nullptr;
        for (const fastgltf::Attribute& attribute : source.instancingAttributes)
        {
            if (attribute.accessorIndex >= m_gltfAsset.accessors.size())
            {
                m_errors.Fail("GPU instancing references an invalid accessor");
            }
            else
            {
                const fastgltf::Accessor&    accessor = m_gltfAsset.accessors[attribute.accessorIndex];
                const fastgltf::AccessorType expected =
                    attribute.name == "ROTATION" ? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;
                if ((attribute.name == "TRANSLATION" || attribute.name == "ROTATION" || attribute.name == "SCALE")
                    && accessor.type != expected)
                {
                    m_errors.Fail("GPU instancing transform accessor has an invalid type");
                }
                else if (instanceCount != 0 && instanceCount != accessor.count)
                {
                    m_errors.Fail("GPU instancing attributes have different instance counts");
                }
                else
                {
                    instanceCount = accessor.count;
                    if (attribute.name == "TRANSLATION")
                    {
                        translations = &accessor;
                    }
                    else if (attribute.name == "ROTATION")
                    {
                        rotations = &accessor;
                    }
                    else if (attribute.name == "SCALE")
                    {
                        scales = &accessor;
                    }
                }
            }
        }
        GeometryReservation parentStorage(m_errors.GetGeometryBudget());
        const bool          admitted =
            m_errors.ReserveGeometryBytes(parentStorage, scene.nodes[nodeIndex].morphWeights.size(), sizeof(float));
        NodeAsset parent;
        if (admitted)
        {
            parent = scene.nodes[nodeIndex];
        }
        std::vector<NodeId> children;
        for (uint32_t instance = 0; instance < instanceCount && !m_errors.Failed(); ++instance)
        {
            NodeAsset node;
            node.name   = parent.name + "_instance_" + std::to_string(instance);
            node.parent = NodeId{nodeIndex};
            node.mesh   = parent.mesh;
            node.skin   = parent.skin;
            if (ReserveGeometryVector(node.morphWeights, parent.morphWeights.size(), m_errors))
            {
                node.morphWeights = parent.morphWeights;
            }
            node.morphWeightsSource = NodeId{nodeIndex};
            node.translation        = DecodeAttribute(m_gltfAsset, translations, instance, Vec3(0));
            node.scale              = DecodeAttribute(m_gltfAsset, scales, instance, Vec3(1));
            const Vec4 rotation     = DecodeAttribute(m_gltfAsset, rotations, instance, Vec4(0, 0, 0, 1));
            node.rotation           = Quat(rotation.w, rotation.x, rotation.y, rotation.z);
            children.push_back(NodeId{static_cast<uint32_t>(scene.nodes.size())});
            scene.nodes.push_back(std::move(node));
        }
        children.insert(children.end(), parent.children.begin(), parent.children.end());
        scene.nodes[nodeIndex].children = std::move(children);
    }
}
} // namespace zenasset
