#pragma once
#include "ZenAsset/Format/Scene.h"
#include "ZenAsset/Format/Status.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zenasset::evaluation
{
class RasterProbe
{
public:
    static constexpr uint32_t Width  = 256;
    static constexpr uint32_t Height = 256;

    RasterProbe();
    ~RasterProbe();
    RasterProbe(const RasterProbe&)            = delete;
    RasterProbe& operator=(const RasterProbe&) = delete;

    Status             Initialize(const std::string& vertexShader, const std::string& fragmentShader);
    Status             Render(std::span<const Vertex>   vertices,
                              std::span<const uint32_t> indices,
                              bool                      lastVertex,
                              std::vector<uint8_t>&     rgba);
    bool               SupportsLastVertex() const;
    const std::string& DeviceName() const;
    uint32_t           ValidationErrorCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace zenasset::evaluation
