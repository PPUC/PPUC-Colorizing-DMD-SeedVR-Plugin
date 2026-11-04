#pragma once

// The pictures the upscaler sends and gets back, and how a result becomes a picture the editor
// takes: no editor and no service here, so that all of it can be tested on its own.

#include <cstdint>
#include <string>
#include <vector>

namespace seedvr {

// RGB565 to 8 bits per channel, the bits repeated (as libserum and the editor show them).
void fromRgb565(uint16_t color, uint8_t& red, uint8_t& green, uint8_t& blue);
// To RGB565, rounded to the nearest level.
[[nodiscard]] uint16_t toRgb565(uint8_t red, uint8_t green, uint8_t blue);

// Which colors an upscaled pixel may take.
enum class ColorMode {
    // Any RGB565 color: the result rounded.
    Any,
    // A color of the source picture's own pixels.
    Picture,
    // A color of the source pixels around the one under it (3x3); the picture's colors where
    // none of them is the picture's own. Keeps edges crisp and invents no color.
    Neighbourhood,
};

[[nodiscard]] const char* colorModeName(ColorMode mode);
// Any for a name it does not know.
[[nodiscard]] ColorMode colorModeFromName(const std::string& name);

// One picture to upscale and where its result goes. Sizes in pixels; pictures row-major.
struct SourcePicture {
    // CDMDPI_PICTURE_... and the record's index, and the name in the review window.
    uint32_t kind = 0;
    uint32_t index = 0;
    std::string label;

    uint32_t width = 0;
    uint32_t height = 0;
    // RGB565, width * height.
    std::vector<uint16_t> pixels;
    // 1 where a pixel is the picture's own; 0 where something else shows (the frame's
    // background, outside a sprite). Empty: all are its own.
    std::vector<uint8_t> own;
    // What shows where the picture's pixels are not its own (the background), RGB565, sent
    // in their place so that the upscaler sees what the author sees. Empty: those pixels take
    // the color of the nearest own pixel instead, so that nothing foreign bleeds in.
    std::vector<uint16_t> context;

    // The picture the result goes into (the HD plane, or the HD sprite), and where the
    // upscaled picture lies in it.
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t offsetX = 0;
    uint32_t offsetY = 0;
    // targetWidth * targetHeight: 1 where the result may be written. Empty: everywhere in the
    // upscaled rectangle.
    std::vector<uint8_t> write;
};

// What is sent for the picture: RGB, 3 bytes a pixel.
[[nodiscard]] std::vector<uint8_t> inputRgb(const SourcePicture& source);

// A picture for the editor's review: RGB565 of the target's size and where to write it.
struct Proposal {
    uint32_t kind = 0;
    uint32_t index = 0;
    std::string label;
    std::vector<uint16_t> pixels;
    std::vector<uint8_t> write;
};

// The result (RGB, scale times the source's size) as a picture for the editor: brought to
// RGB565 with the color mode, placed into the target, written only where the source allows.
// Throws std::invalid_argument for a result of another size.
[[nodiscard]] Proposal makeProposal(const SourcePicture& source,
                                    const std::vector<uint8_t>& rgb,
                                    uint32_t width,
                                    uint32_t height,
                                    ColorMode colors);

// Base64 (RFC 4648, with padding), for the service's JSON.
[[nodiscard]] std::string base64Encode(const std::vector<uint8_t>& bytes);
// Throws std::invalid_argument for text that is not base64.
[[nodiscard]] std::vector<uint8_t> base64Decode(const std::string& text);

}  // namespace seedvr
