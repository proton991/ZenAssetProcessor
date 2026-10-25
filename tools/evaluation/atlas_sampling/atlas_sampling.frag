#version 450
layout(binding = 0) uniform sampler2D sourceTexture;
layout(binding = 1) uniform sampler2D lowPage;
layout(binding = 2) uniform sampler2D highPage;
layout(binding = 4) uniform sampler2DArray arrayPage;
layout(binding = 3, std430) buffer Output
{
    vec4 values[];
}
outputData;
layout(push_constant) uniform Parameters
{
    vec4 lowRegion;
    vec4 highRegion;
    vec4 query; // LOD, mode (0 explicit, 1 ideal gradient, 2 hardware query), grid side, max logical LOD
    vec4 gradients;
    vec4 settings; // mipmap precision bits, quantize blend, bound low level, reserved
    vec4 sampling; // coordinate grid, subtexel precision bits, reserved, reserved
}
p;

vec4 TileBilinear(vec2 uv, bool quantized)
{
    // Keep the footprint in tile coordinates until the integer texel fetches.
    // Quantization is a diagnostic candidate, not a claim of portable hardware
    // rounding equivalence from subTexelPrecisionBits alone.
    precise vec2 scaled  = uv * p.lowRegion.zw;
    precise vec2 local   = scaled - 0.5;
    ivec2        first   = ivec2(floor(local));
    ivec2        last    = ivec2(p.lowRegion.zw) - 1;
    ivec2        origin  = ivec2(p.lowRegion.xy);
    vec2         weights = fract(local);
    if (quantized)
    {
        float divisions = exp2(p.sampling.y);
        weights         = roundEven(weights * divisions) / divisions;
    }
    vec4 c00 = texelFetch(lowPage, origin + clamp(first, ivec2(0), last), 0);
    vec4 c10 = texelFetch(lowPage, origin + clamp(first + ivec2(1, 0), ivec2(0), last), 0);
    vec4 c01 = texelFetch(lowPage, origin + clamp(first + ivec2(0, 1), ivec2(0), last), 0);
    vec4 c11 = texelFetch(lowPage, origin + clamp(first + ivec2(1, 1), ivec2(0), last), 0);
    return mix(mix(c00, c10, weights.x), mix(c01, c11, weights.x), weights.y);
}

#if ZENASSET_PROBE_FLOAT64
vec2 DoubleRoundedUV(vec2 uv, vec4 region, vec2 pageSize)
{
    // A diagnostic: multiply the original float UV exactly in the wider domain
    // before fixed-point rounding. Page coordinates then use exactly representable
    // fractions when the page dimensions are powers of two.
    double divisions = double(exp2(p.sampling.y));
    dvec2  original  = dvec2(uv);
    if (p.settings.w == 13.0)
    {
        // The 1D observations motivate testing a normalized-coordinate fixed
        // point conversion before the texel-domain conversion. This is a probe,
        // not an assumed Vulkan guarantee or a production device rule.
        original = floor(original * 16777216.0) / 16777216.0;
    }
    precise dvec2 scaled  = original * dvec2(region.zw) * divisions;
    dvec2         rounded = p.settings.w == 10.0 ? roundEven(scaled) : floor(scaled + 0.5);
    precise vec2  local   = vec2(rounded / divisions);
    precise vec2  offset  = region.xy + local;
    precise vec2  result  = offset / pageSize;
    return result;
}
#endif

void main()
{
    uvec2 cell = uvec2(gl_FragCoord.xy);
    uint  side = uint(p.query.z);
    vec2  uv   = vec2(cell) / float(side - 1);
    if (p.sampling.x == 1.0)
    {
        uv = (vec2(cell) + 0.5) / float(side);
    }
    else if (p.sampling.x == 2.0)
    {
        uint seed = cell.y * side + cell.x;
        uv        = vec2((seed * 251u + 17u) % 65521u, (seed * 509u + 29u) % 65521u) / 65520.0;
    }
    vec2 dimensions = vec2(textureSize(sourceTexture, 0));
    if (p.sampling.x >= 3.0)
    {
        // Exercise native fixed-point rounding boundaries, and the immediately
        // adjacent representable normalized coordinates, in both dimensions.
        float divisions = exp2(p.sampling.y);
        vec2  fraction  = (mod(vec2(cell) * 2.0, divisions) + 0.5) / divisions;
        vec2  texel     = mod(vec2(cell.y, cell.x) * vec2(13.0, 17.0), dimensions);
        uv              = min((texel + 0.5 + fraction) / dimensions, vec2(1.0));
        if (p.sampling.x == 4.0)
        {
            uv = uintBitsToFloat(floatBitsToUint(uv) - uvec2(1u));
        }
        else if (p.sampling.x == 5.0)
        {
            uv = min(uintBitsToFloat(floatBitsToUint(uv) + uvec2(1u)), vec2(1.0));
        }
    }
    vec2  dx       = p.gradients.xy * dimensions;
    vec2  dy       = p.gradients.zw * dimensions;
    float idealLOD = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-20));
    // Affine coordinates supply the specified derivatives to QueryLod in the
    // fragment stage. The sampled UV grid is independent so every case includes
    // boundaries. Query against the physical page after correcting its extent.
    vec2  lodUV     = p.gradients.xy * gl_FragCoord.x + p.gradients.zw * gl_FragCoord.y;
    float sourceLOD = textureQueryLod(sourceTexture, lodUV).y;
    float pageLOD   = textureQueryLod(lowPage, lodUV * dimensions / vec2(textureSize(lowPage, 0))).y;
    float lod       = p.query.y == 0.0 ? p.query.x : p.query.y == 1.0 ? idealLOD : pageLOD;
    vec4  reference = p.query.y == 0.0 ? textureLod(sourceTexture, uv, p.query.x)
                                       : textureGrad(sourceTexture, uv, p.gradients.xy, p.gradients.zw);
    lod             = clamp(lod, 0.0, p.query.w);
    vec2 lowUV      = (p.lowRegion.xy + uv * p.lowRegion.zw) / vec2(textureSize(lowPage, 0));
    vec2 highUV     = (p.highRegion.xy + uv * p.highRegion.zw) / vec2(textureSize(highPage, 0));
    if (p.settings.w == 5.0)
    {
        precise vec2 lowPixel       = uv * p.lowRegion.zw;
        precise vec2 lowOffset      = p.lowRegion.xy + lowPixel;
        precise vec2 lowNormalized  = lowOffset / vec2(textureSize(lowPage, 0));
        precise vec2 highPixel      = uv * p.highRegion.zw;
        precise vec2 highOffset     = p.highRegion.xy + highPixel;
        precise vec2 highNormalized = highOffset / vec2(textureSize(highPage, 0));
        lowUV                       = lowNormalized;
        highUV                      = highNormalized;
    }
    if (p.settings.w == 9.0 || p.settings.w == 12.0 || p.settings.w == 14.0)
    {
        // Probe whether quantizing in the original image's coordinate domain
        // retains native filtering while avoiding a page-offset precision loss.
        float        divisions      = exp2(p.sampling.y);
        precise vec2 lowScaled      = uv * p.lowRegion.zw;
        precise vec2 highScaled     = uv * p.highRegion.zw;
        precise vec2 lowRounded     = (p.settings.w == 14.0   ? floor(lowScaled * divisions)
                                       : p.settings.w == 12.0 ? floor(lowScaled * divisions + 0.5)
                                                              : roundEven(lowScaled * divisions))
                                    / divisions;
        precise vec2 highRounded    = (p.settings.w == 14.0   ? floor(highScaled * divisions)
                                       : p.settings.w == 12.0 ? floor(highScaled * divisions + 0.5)
                                                              : roundEven(highScaled * divisions))
                                    / divisions;
        precise vec2 lowOffset      = p.lowRegion.xy + lowRounded;
        precise vec2 highOffset     = p.highRegion.xy + highRounded;
        precise vec2 lowNormalized  = lowOffset / vec2(textureSize(lowPage, 0));
        precise vec2 highNormalized = highOffset / vec2(textureSize(highPage, 0));
        lowUV                       = lowNormalized;
        highUV                      = highNormalized;
    }
#if ZENASSET_PROBE_FLOAT64
    if (p.settings.w == 10.0 || p.settings.w == 11.0 || p.settings.w == 13.0)
    {
        lowUV  = DoubleRoundedUV(uv, p.lowRegion, vec2(textureSize(lowPage, 0)));
        highUV = DoubleRoundedUV(uv, p.highRegion, vec2(textureSize(highPage, 0)));
    }
#endif
    float blend = fract(lod);
    if (p.settings.y != 0.0)
    {
        float divisions = exp2(p.settings.x);
        blend           = round(blend * divisions) / divisions;
    }
    vec4 pageLow    = textureLod(lowPage, lowUV, p.settings.w != 0.0 ? p.settings.z : 0.0);
    vec4 pageHigh   = textureLod(highPage, highUV, p.settings.w != 0.0 ? min(p.settings.z + 1.0, p.query.w) : 0.0);
    vec4 nativeLow  = textureLod(sourceTexture, uv, p.settings.z);
    vec4 nativeHigh = textureLod(sourceTexture, uv, min(p.settings.z + 1.0, p.query.w));
    vec4 packed     = mix(pageLow, pageHigh, blend);
    if (p.settings.w == 1.0 || p.settings.w == 4.0 || p.settings.w == 5.0 || p.settings.w >= 9.0)
    {
        // Preserve hardware filtering through an actual guarded native mip chain.
        packed = p.query.y == 0.0 ? textureLod(lowPage, lowUV, p.query.x)
                                  : textureGrad(lowPage, lowUV, p.gradients.xy * dimensions / vec2(textureSize(lowPage, 0)),
                                                p.gradients.zw * dimensions / vec2(textureSize(lowPage, 0)));
    }
    if (p.settings.w == 2.0)
    {
        pageLow  = textureLod(arrayPage, vec3(uv, 1.0), p.settings.z);
        pageHigh = textureLod(arrayPage, vec3(uv, 1.0), min(p.settings.z + 1.0, p.query.w));
        packed   = p.query.y == 0.0 ? textureLod(arrayPage, vec3(uv, 1.0), p.query.x)
                                    : textureGrad(arrayPage, vec3(uv, 1.0), p.gradients.xy, p.gradients.zw);
    }
    if (p.settings.w == 3.0 || p.settings.w == 6.0)
    {
        pageLow  = textureLod(lowPage, uv, p.settings.z);
        pageHigh = textureLod(lowPage, uv, min(p.settings.z + 1.0, p.query.w));
        packed =
            p.query.y == 0.0 ? textureLod(lowPage, uv, p.query.x) : textureGrad(lowPage, uv, p.gradients.xy, p.gradients.zw);
    }
    if (p.settings.w == 7.0 || p.settings.w == 8.0)
    {
        packed  = TileBilinear(uv, p.settings.w == 8.0);
        pageLow = pageHigh = packed;
    }
    vec4 nativeBlend             = mix(nativeLow, nativeHigh, blend);
    vec4 lowError                = abs(pageLow - nativeLow);
    vec4 highError               = abs(pageHigh - nativeHigh);
    uint index                   = (cell.y * side + cell.x) * 5;
    outputData.values[index]     = reference;
    outputData.values[index + 1] = packed;
    outputData.values[index + 2] = vec4(sourceLOD, pageLOD, idealLOD, floor(lod) - p.settings.z);
    outputData.values[index + 3] = nativeBlend;
    outputData.values[index + 4] = vec4(max(max(lowError.x, lowError.y), max(lowError.z, lowError.w)),
                                        max(max(highError.x, highError.y), max(highError.z, highError.w)), uv.x, uv.y);
}
