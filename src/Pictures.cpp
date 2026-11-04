#include "Pictures.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace seedvr {

namespace {

// OKLab (Björn Ottosson), where distances follow how different colors look.
struct Lab {
    float l = 0;
    float a = 0;
    float b = 0;
};

float linear(uint8_t channel)
{
    const float value = static_cast<float>(channel) / 255.0f;
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

Lab toLab(uint8_t red, uint8_t green, uint8_t blue)
{
    const float r = linear(red);
    const float g = linear(green);
    const float b = linear(blue);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return Lab{0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
               1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
               0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

Lab toLab(uint16_t color)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    fromRgb565(color, red, green, blue);
    return toLab(red, green, blue);
}

float distance(const Lab& x, const Lab& y)
{
    const float dl = x.l - y.l;
    const float da = x.a - y.a;
    const float db = x.b - y.b;
    return dl * dl + da * da + db * db;
}

// The candidate nearest to the color; the color rounded without candidates.
uint16_t nearest(const Lab& color, const std::vector<uint16_t>& candidates,
                 const std::vector<Lab>& labs, uint16_t fallback)
{
    if (candidates.empty()) {
        return fallback;
    }
    std::size_t best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const float d = distance(color, labs[index]);
        if (d < bestDistance) {
            bestDistance = d;
            best = index;
        }
    }
    return candidates[best];
}

bool isOwn(const SourcePicture& source, std::size_t index)
{
    return source.own.empty() || source.own[index] != 0;
}

void checkSource(const SourcePicture& source)
{
    const std::size_t count = static_cast<std::size_t>(source.width) * source.height;
    if (count == 0 || source.pixels.size() != count ||
        (!source.own.empty() && source.own.size() != count) ||
        (!source.context.empty() && source.context.size() != count)) {
        throw std::invalid_argument("the source picture's parts have different sizes");
    }
}

}  // namespace

void fromRgb565(uint16_t color, uint8_t& red, uint8_t& green, uint8_t& blue)
{
    const unsigned r = (color >> 11) & 0x1f;
    const unsigned g = (color >> 5) & 0x3f;
    const unsigned b = color & 0x1f;
    red = static_cast<uint8_t>((r << 3) | (r >> 2));
    green = static_cast<uint8_t>((g << 2) | (g >> 4));
    blue = static_cast<uint8_t>((b << 3) | (b >> 2));
}

uint16_t toRgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    const unsigned r = (static_cast<unsigned>(red) * 31 + 127) / 255;
    const unsigned g = (static_cast<unsigned>(green) * 63 + 127) / 255;
    const unsigned b = (static_cast<unsigned>(blue) * 31 + 127) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

const char* colorModeName(ColorMode mode)
{
    switch (mode) {
    case ColorMode::Any:
        return "any";
    case ColorMode::Picture:
        return "picture";
    case ColorMode::Neighbourhood:
        return "neighbourhood";
    }
    return "any";
}

ColorMode colorModeFromName(const std::string& name)
{
    if (name == "picture") {
        return ColorMode::Picture;
    }
    if (name == "neighbourhood") {
        return ColorMode::Neighbourhood;
    }
    return ColorMode::Any;
}

std::vector<uint8_t> inputRgb(const SourcePicture& source)
{
    checkSource(source);
    const int width = static_cast<int>(source.width);
    const int height = static_cast<int>(source.height);
    std::vector<uint16_t> colors(source.pixels);
    if (!source.own.empty()) {
        bool anyOwn = false;
        for (uint8_t own : source.own) {
            anyOwn = anyOwn || own != 0;
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                if (source.own[index] != 0) {
                    continue;
                }
                if (!source.context.empty()) {
                    colors[index] = source.context[index];
                    continue;
                }
                if (!anyOwn) {
                    continue;
                }
                // The nearest own pixel (city block distance; the first found on a tie).
                int bestDistance = std::numeric_limits<int>::max();
                for (int oy = 0; oy < height; ++oy) {
                    for (int ox = 0; ox < width; ++ox) {
                        const std::size_t other = static_cast<std::size_t>(oy) * width + ox;
                        const int d = std::abs(ox - x) + std::abs(oy - y);
                        if (source.own[other] != 0 && d < bestDistance) {
                            bestDistance = d;
                            colors[index] = source.pixels[other];
                        }
                    }
                }
            }
        }
    }
    std::vector<uint8_t> rgb(colors.size() * 3);
    for (std::size_t index = 0; index < colors.size(); ++index) {
        fromRgb565(colors[index], rgb[index * 3], rgb[index * 3 + 1], rgb[index * 3 + 2]);
    }
    return rgb;
}

Proposal makeProposal(const SourcePicture& source,
                      const std::vector<uint8_t>& rgb,
                      uint32_t width,
                      uint32_t height,
                      ColorMode colors)
{
    checkSource(source);
    if (width == 0 || height == 0 || width % source.width != 0 ||
        height % source.height != 0 || width / source.width != height / source.height ||
        rgb.size() != static_cast<std::size_t>(width) * height * 3) {
        throw std::invalid_argument("the result has not a whole multiple of the source's size");
    }
    const uint32_t scale = width / source.width;
    const std::size_t targetCount = static_cast<std::size_t>(source.targetWidth) *
                                    source.targetHeight;
    if (source.offsetX + width > source.targetWidth ||
        source.offsetY + height > source.targetHeight ||
        (!source.write.empty() && source.write.size() != targetCount)) {
        throw std::invalid_argument("the result does not fit the target");
    }

    // The picture's own colors, for the color modes.
    std::vector<uint16_t> palette;
    {
        std::set<uint16_t> unique;
        for (std::size_t index = 0; index < source.pixels.size(); ++index) {
            if (isOwn(source, index)) {
                unique.insert(source.pixels[index]);
            }
        }
        palette.assign(unique.begin(), unique.end());
    }
    std::vector<Lab> paletteLabs;
    for (uint16_t color : palette) {
        paletteLabs.push_back(toLab(color));
    }

    Proposal proposal;
    proposal.kind = source.kind;
    proposal.index = source.index;
    proposal.label = source.label;
    proposal.pixels.assign(targetCount, 0);
    proposal.write.assign(targetCount, 0);
    std::vector<uint16_t> around;
    std::vector<Lab> aroundLabs;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t target =
                static_cast<std::size_t>(source.offsetY + y) * source.targetWidth +
                source.offsetX + x;
            if (!source.write.empty() && source.write[target] == 0) {
                continue;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
            const uint16_t rounded = toRgb565(rgb[at], rgb[at + 1], rgb[at + 2]);
            uint16_t color = rounded;
            if (colors != ColorMode::Any) {
                const Lab lab = toLab(rgb[at], rgb[at + 1], rgb[at + 2]);
                bool done = false;
                if (colors == ColorMode::Neighbourhood) {
                    around.clear();
                    aroundLabs.clear();
                    const int sx = static_cast<int>(x / scale);
                    const int sy = static_cast<int>(y / scale);
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = sx + dx;
                            const int ny = sy + dy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<int>(source.width) ||
                                ny >= static_cast<int>(source.height)) {
                                continue;
                            }
                            const std::size_t index =
                                static_cast<std::size_t>(ny) * source.width + nx;
                            if (isOwn(source, index)) {
                                around.push_back(source.pixels[index]);
                                aroundLabs.push_back(toLab(source.pixels[index]));
                            }
                        }
                    }
                    if (!around.empty()) {
                        color = nearest(lab, around, aroundLabs, rounded);
                        done = true;
                    }
                }
                if (!done) {
                    color = nearest(lab, palette, paletteLabs, rounded);
                }
            }
            proposal.pixels[target] = color;
            proposal.write[target] = 1;
        }
    }
    return proposal;
}

std::string base64Encode(const std::vector<uint8_t>& bytes)
{
    static const char* const kDigits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[index]) << 16) |
                               (index + 1 < bytes.size() ? bytes[index + 1] << 8 : 0) |
                               (index + 2 < bytes.size() ? bytes[index + 2] : 0);
        text += kDigits[(chunk >> 18) & 63];
        text += kDigits[(chunk >> 12) & 63];
        text += index + 1 < bytes.size() ? kDigits[(chunk >> 6) & 63] : '=';
        text += index + 2 < bytes.size() ? kDigits[chunk & 63] : '=';
    }
    return text;
}

std::vector<uint8_t> base64Decode(const std::string& text)
{
    std::array<int, 256> values{};
    values.fill(-1);
    const std::string digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < digits.size(); ++index) {
        values[static_cast<unsigned char>(digits[index])] = static_cast<int>(index);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3);
    uint32_t chunk = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (char character : text) {
        if (character == '=') {
            ++padding;
            continue;
        }
        const int value = values[static_cast<unsigned char>(character)];
        if (value < 0 || padding > 0) {
            throw std::invalid_argument("not base64");
        }
        chunk = (chunk << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>((chunk >> bits) & 0xff));
        }
    }
    if (padding > 2) {
        throw std::invalid_argument("not base64");
    }
    return bytes;
}

}  // namespace seedvr
