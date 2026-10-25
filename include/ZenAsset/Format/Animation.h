#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zenasset
{
enum class AnimatedObject : uint32_t
{
    Node,
    Material,
    Camera,
    Light,
    Environment
};

enum class AnimatedProperty : uint32_t
{
    Invalid,
    Translation,
    Rotation,
    Scale,
    Weights,
    Visible,
    Selectable,
    Hoverable,
    BaseColor,
    Metallic,
    Roughness,
    Emissive,
    AlphaCutoff,
    NormalScale,
    OcclusionStrength,
    EmissiveStrength,
    Ior,
    Dispersion,
    Specular,
    SpecularColor,
    Diffuse,
    SpecularGlossinessColor,
    Glossiness,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormalScale,
    SheenColor,
    SheenRoughness,
    Transmission,
    Thickness,
    AttenuationDistance,
    AttenuationColor,
    Iridescence,
    IridescenceIor,
    IridescenceThicknessMinimum,
    IridescenceThicknessMaximum,
    Anisotropy,
    AnisotropyRotation,
    DiffuseTransmission,
    DiffuseTransmissionColor,
    MultiscatterColor,
    Retroreflection,
    TextureOffset,
    TextureScale,
    TextureRotation,
    CameraAspect,
    CameraFov,
    CameraNear,
    CameraFar,
    CameraXMag,
    CameraYMag,
    LightColor,
    LightIntensity,
    LightRange,
    LightInnerCone,
    LightOuterCone,
    EnvironmentIntensity,
    EnvironmentRotation
};

// A semantic property, resolved once at import. No source path is interpreted at playback.
struct AnimationTarget
{
    AnimatedObject   object{AnimatedObject::Node};
    uint32_t         index{0};
    AnimatedProperty property{AnimatedProperty::Invalid};
    uint32_t         dimensions{0};
    // Only weights support component addressing. UINT32_MAX means the whole value.
    uint32_t element{UINT32_MAX};
    // Core material slots 0..4, then MaterialFeatureTexture slots.
    uint32_t textureSlot{UINT32_MAX};
};

inline bool IsQuaternionTarget(const AnimationTarget& target)
{
    return target.property == AnimatedProperty::Rotation || target.property == AnimatedProperty::EnvironmentRotation;
}
enum class AnimationPath : uint32_t
{
    Translation,
    Rotation,
    Scale,
    Weights,
    Property
};

enum class AnimationInterpolation : uint32_t
{
    Linear,
    Step,
    CubicSpline
};

struct AnimationSampler
{
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    uint32_t               components{0};
    std::vector<float>     times;
    // Cubic splines retain in-tangent, value, out-tangent triples.
    std::vector<float> values;
};

struct AnimationChannel
{
    uint32_t        sampler{0};
    uint32_t        node{0};
    AnimationPath   path{AnimationPath::Translation};
    AnimationTarget target;
    // Optional diagnostic metadata; playback never reads it.
    std::string sourceTarget;
};

struct AnimationBakeSettings
{
    double   sampleRate{60};
    float    translationTolerance{1e-5f};
    float    rotationTolerance{1e-4f};
    float    scaleTolerance{1e-5f};
    float    valueTolerance{1e-4f};
    uint32_t maxSubdivisions{10};
    uint64_t maxValues{64u * 1024u * 1024u};
};

struct AnimationAsset
{
    std::string                   name;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
    bool                          baked{false};
    AnimationBakeSettings         bakeSettings;
};

} // namespace zenasset
