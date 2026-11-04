#pragma once

// An upscaling run: the pictures gathered from the editor (on its main thread), sent to the
// service in jobs of clips, and the results made into proposals for the review window.

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Pictures.h"
#include "ServiceClient.h"

namespace seedvr {

struct Options {
    std::string model = "3b";
    int64_t seed = 42;
    ColorMode colors = ColorMode::Neighbourhood;
    // Frames are sent with their background where it shows through; else those pixels take
    // the nearest frame color.
    bool backgroundInInput = true;
    // Dynamic zones are left as they are (libserum colors them at run time anyway).
    bool skipDynamicZones = false;
    // The most consecutive frames upscaled as one clip.
    uint32_t runLength = 16;
};

// The project cannot be upscaled, or there is nothing to upscale; the text says why, for the
// author.
class UpscaleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// On the editor's main thread. Each throws UpscaleError for a project without an HD plane of
// twice the SD plane's size.
[[nodiscard]] std::vector<SourcePicture> gatherFrames(const ColorizingDMDEditorAPI* api,
                                                      const std::vector<uint32_t>& frames,
                                                      const Options& options);
[[nodiscard]] std::vector<SourcePicture> gatherBackgrounds(
    const ColorizingDMDEditorAPI* api,
    const std::vector<uint32_t>& backgrounds);
// Sprites without any pixel in the SD sprite's area are left out.
[[nodiscard]] std::vector<SourcePicture> gatherSprites(const ColorizingDMDEditorAPI* api,
                                                       const std::vector<uint32_t>& sprites);

// The backgrounds and the sprites of the frames, ascending, each once.
[[nodiscard]] std::vector<uint32_t> backgroundsOf(const ColorizingDMDEditorAPI* api,
                                                  const std::vector<uint32_t>& frames);
[[nodiscard]] std::vector<uint32_t> spritesOf(const ColorizingDMDEditorAPI* api,
                                              const std::vector<uint32_t>& frames);

// The pictures as clips, as indices into sources: consecutive frames together, at most
// runLength of them; every background and sprite alone.
[[nodiscard]] std::vector<std::vector<std::size_t>> sequencesOf(
    const std::vector<SourcePicture>& sources,
    uint32_t runLength);

// The ID a picture has in the service's protocol ("f12", "b0", "s3").
[[nodiscard]] std::string pictureId(const SourcePicture& source);

struct RunHooks {
    // Pictures done of all, as the service reports them.
    std::function<void(uint32_t done, uint32_t total)> progress;
    // Asked between polls; true cancels the job and ends the run with UpscaleError.
    std::function<bool()> cancelled;
    std::chrono::milliseconds pollInterval{500};
};

// Runs the pictures through the service and returns their proposals, in the order of the
// sources. Throws ServiceError when the service fails, UpscaleError when cancelled.
[[nodiscard]] std::vector<Proposal> runUpscale(const ServiceClient& client,
                                               const std::vector<SourcePicture>& sources,
                                               const Options& options,
                                               const RunHooks& hooks);

}  // namespace seedvr
