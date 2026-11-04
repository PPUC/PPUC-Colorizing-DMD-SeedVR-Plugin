#include "Upscale.h"

#include <algorithm>
#include <map>
#include <set>
#include <thread>

namespace seedvr {

namespace {

template <typename T, typename Read>
std::vector<T> readAll(Read&& read)
{
    std::vector<T> values(read(nullptr, 0));
    if (!values.empty()) {
        read(values.data(), static_cast<uint32_t>(values.size()));
    }
    return values;
}

ColorizingDMDProjectInfo requireHd(const ColorizingDMDEditorAPI* api)
{
    if (!api || api->version < 3) {
        throw UpscaleError("The editor is too old for the upscaler: it needs editor API "
                           "version 3.");
    }
    ColorizingDMDProjectInfo project{};
    if (!api->GetProjectInfo(&project) || !project.hasProject) {
        throw UpscaleError("No project is open.");
    }
    if (project.hdWidth != project.sdWidth * 2 || project.hdHeight != project.sdHeight * 2) {
        throw UpscaleError("The upscaler needs a project with an HD plane of twice the SD "
                           "plane's size.");
    }
    return project;
}

// The mask of the larger plane from the smaller one: each pixel takes the one under it.
std::vector<uint8_t> doubled(const std::vector<uint8_t>& mask, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> result(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height * 2; ++y) {
        for (uint32_t x = 0; x < width * 2; ++x) {
            result[static_cast<std::size_t>(y) * width * 2 + x] =
                mask[static_cast<std::size_t>(y / 2) * width + x / 2];
        }
    }
    return result;
}

std::vector<uint8_t> inverted(const std::vector<uint8_t>& mask)
{
    std::vector<uint8_t> result(mask.size());
    for (std::size_t index = 0; index < mask.size(); ++index) {
        result[index] = mask[index] != 0 ? 0 : 1;
    }
    return result;
}

}  // namespace

std::vector<SourcePicture> gatherFrames(const ColorizingDMDEditorAPI* api,
                                        const std::vector<uint32_t>& frames,
                                        const Options& options)
{
    const ColorizingDMDProjectInfo project = requireHd(api);
    const uint32_t width = project.sdWidth;
    const uint32_t height = project.sdHeight;
    std::vector<SourcePicture> sources;
    for (uint32_t frame : frames) {
        ColorizingDMDFrameInfo info{};
        if (!api->GetFrameInfo(frame, &info)) {
            continue;
        }
        SourcePicture source;
        source.kind = CDMDPI_PICTURE_FRAME;
        source.index = frame;
        source.width = width;
        source.height = height;
        source.pixels = readAll<uint16_t>([&](uint16_t* buffer, uint32_t capacity) {
            return api->GetColorizedFrame(frame, 0, buffer, capacity, nullptr, nullptr);
        });
        if (source.pixels.size() != static_cast<std::size_t>(width) * height) {
            continue;
        }
        source.targetWidth = width * 2;
        source.targetHeight = height * 2;

        const std::vector<uint8_t> background = readAll<uint8_t>(
            [&](uint8_t* buffer, uint32_t capacity) {
                return api->GetBackgroundMask(frame, 0, buffer, capacity, nullptr, nullptr);
            });
        const bool showsBackground =
            std::any_of(background.begin(), background.end(), [](uint8_t v) { return v; });
        if (showsBackground) {
            source.own = inverted(background);
            if (options.backgroundInInput && info.background != CDMDPI_NONE) {
                source.context = readAll<uint16_t>([&](uint16_t* buffer, uint32_t capacity) {
                    return api->GetBackgroundPicture(info.background, 0, buffer, capacity,
                                                     nullptr, nullptr);
                });
                if (source.context.size() != source.pixels.size()) {
                    source.context.clear();
                }
            }
            // On the HD plane as the frame has it, or as Create HD will double it.
            source.write = info.hasHd ? inverted(readAll<uint8_t>(
                                            [&](uint8_t* buffer, uint32_t capacity) {
                                                return api->GetBackgroundMask(
                                                    frame, 1, buffer, capacity, nullptr,
                                                    nullptr);
                                            }))
                                      : doubled(source.own, width, height);
        }
        if (options.skipDynamicZones && info.hasDynamicPixels) {
            std::vector<uint8_t> zones = readAll<uint8_t>([&](uint8_t* buffer, uint32_t cap) {
                return api->GetDynamicMask(frame, buffer, cap, nullptr, nullptr);
            });
            for (uint8_t& set : zones) {
                set = set == 255 ? 1 : 0;
            }
            if (!project.dynamicPlaneIsHd) {
                zones = doubled(zones, width, height);
            }
            if (source.write.empty()) {
                source.write = zones;
            } else if (zones.size() == source.write.size()) {
                for (std::size_t index = 0; index < zones.size(); ++index) {
                    source.write[index] = source.write[index] && zones[index] ? 1 : 0;
                }
            }
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

std::vector<SourcePicture> gatherBackgrounds(const ColorizingDMDEditorAPI* api,
                                             const std::vector<uint32_t>& backgrounds)
{
    const ColorizingDMDProjectInfo project = requireHd(api);
    std::vector<SourcePicture> sources;
    for (uint32_t background : backgrounds) {
        SourcePicture source;
        source.kind = CDMDPI_PICTURE_BACKGROUND;
        source.index = background;
        source.width = project.sdWidth;
        source.height = project.sdHeight;
        source.pixels = readAll<uint16_t>([&](uint16_t* buffer, uint32_t capacity) {
            return api->GetBackgroundPicture(background, 0, buffer, capacity, nullptr,
                                             nullptr);
        });
        if (source.pixels.size() != static_cast<std::size_t>(source.width) * source.height) {
            continue;
        }
        source.targetWidth = project.hdWidth;
        source.targetHeight = project.hdHeight;
        sources.push_back(std::move(source));
    }
    return sources;
}

std::vector<SourcePicture> gatherSprites(const ColorizingDMDEditorAPI* api,
                                         const std::vector<uint32_t>& sprites)
{
    static_cast<void>(requireHd(api));
    const uint32_t width = CDMDPI_SPRITE_WIDTH;
    const uint32_t height = CDMDPI_SPRITE_HEIGHT;
    std::vector<SourcePicture> sources;
    for (uint32_t sprite : sprites) {
        const std::vector<uint16_t> colors =
            readAll<uint16_t>([&](uint16_t* buffer, uint32_t capacity) {
                return api->GetSpritePicture(sprite, 0, buffer, capacity, nullptr, nullptr);
            });
        const std::vector<uint8_t> shape = readAll<uint8_t>([&](uint8_t* buffer, uint32_t cap) {
            return api->GetSpriteMask(sprite, 0, buffer, cap, nullptr, nullptr);
        });
        if (colors.size() != static_cast<std::size_t>(width) * height ||
            shape.size() != colors.size()) {
            continue;
        }
        // The SD sprite fills the upper left quarter: the part of it the sprite covers.
        uint32_t left = width;
        uint32_t top = height;
        uint32_t right = 0;
        uint32_t bottom = 0;
        for (uint32_t y = 0; y < height / 2; ++y) {
            for (uint32_t x = 0; x < width / 2; ++x) {
                if (shape[static_cast<std::size_t>(y) * width + x]) {
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x + 1);
                    bottom = std::max(bottom, y + 1);
                }
            }
        }
        if (right <= left || bottom <= top) {
            continue;
        }
        SourcePicture source;
        source.kind = CDMDPI_PICTURE_SPRITE;
        source.index = sprite;
        source.width = right - left;
        source.height = bottom - top;
        for (uint32_t y = top; y < bottom; ++y) {
            for (uint32_t x = left; x < right; ++x) {
                source.pixels.push_back(colors[static_cast<std::size_t>(y) * width + x]);
                source.own.push_back(shape[static_cast<std::size_t>(y) * width + x]);
            }
        }
        source.targetWidth = width;
        source.targetHeight = height;
        source.offsetX = left * 2;
        source.offsetY = top * 2;
        // Where the HD sprite covers, or will once Create HD doubles the SD one.
        std::vector<uint8_t> hdShape = readAll<uint8_t>([&](uint8_t* buffer, uint32_t cap) {
            return api->GetSpriteMask(sprite, 1, buffer, cap, nullptr, nullptr);
        });
        if (hdShape.size() != colors.size()) {
            hdShape.assign(colors.size(), 0);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    hdShape[static_cast<std::size_t>(y) * width + x] =
                        shape[static_cast<std::size_t>(y / 2) * width + x / 2];
                }
            }
        }
        source.write = std::move(hdShape);
        sources.push_back(std::move(source));
    }
    return sources;
}

std::vector<uint32_t> backgroundsOf(const ColorizingDMDEditorAPI* api,
                                    const std::vector<uint32_t>& frames)
{
    std::set<uint32_t> backgrounds;
    for (uint32_t frame : frames) {
        ColorizingDMDFrameInfo info{};
        if (api->GetFrameInfo(frame, &info) && info.background != CDMDPI_NONE) {
            backgrounds.insert(info.background);
        }
    }
    return {backgrounds.begin(), backgrounds.end()};
}

std::vector<uint32_t> spritesOf(const ColorizingDMDEditorAPI* api,
                                const std::vector<uint32_t>& frames)
{
    std::set<uint32_t> sprites;
    for (uint32_t frame : frames) {
        for (uint32_t sprite : readAll<uint32_t>([&](uint32_t* buffer, uint32_t capacity) {
                 return api->GetFrameSprites(frame, buffer, capacity);
             })) {
            sprites.insert(sprite);
        }
    }
    return {sprites.begin(), sprites.end()};
}

std::vector<std::vector<std::size_t>> sequencesOf(const std::vector<SourcePicture>& sources,
                                                  uint32_t runLength)
{
    const std::size_t longest = std::max<uint32_t>(runLength, 1);
    std::vector<std::vector<std::size_t>> sequences;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const SourcePicture& source = sources[index];
        const bool continues =
            source.kind == CDMDPI_PICTURE_FRAME && !sequences.empty() &&
            sequences.back().size() < longest &&
            sources[sequences.back().back()].kind == CDMDPI_PICTURE_FRAME &&
            sources[sequences.back().back()].index + 1 == source.index;
        if (continues) {
            sequences.back().push_back(index);
        } else {
            sequences.push_back({index});
        }
    }
    return sequences;
}

std::string pictureId(const SourcePicture& source)
{
    const char* prefix = source.kind == CDMDPI_PICTURE_BACKGROUND ? "b"
                         : source.kind == CDMDPI_PICTURE_SPRITE   ? "s"
                                                                  : "f";
    return prefix + std::to_string(source.index);
}

std::vector<Proposal> runUpscale(const ServiceClient& client,
                                 const std::vector<SourcePicture>& sources,
                                 const Options& options,
                                 const RunHooks& hooks)
{
    if (sources.empty()) {
        throw UpscaleError("There is nothing to upscale.");
    }
    const ServiceInfo info = client.info();
    const std::size_t perJob = info.maxFrames > 0 ? info.maxFrames : sources.size();

    // Jobs of whole clips, as many as the service takes; a longer clip is split.
    std::vector<std::vector<std::vector<std::size_t>>> jobs(1);
    std::size_t inJob = 0;
    for (const std::vector<std::size_t>& sequence : sequencesOf(sources, options.runLength)) {
        for (std::size_t start = 0; start < sequence.size(); start += perJob) {
            const std::size_t end = std::min(sequence.size(), start + perJob);
            if (inJob + (end - start) > perJob) {
                jobs.emplace_back();
                inJob = 0;
            }
            jobs.back().emplace_back(sequence.begin() + static_cast<std::ptrdiff_t>(start),
                                     sequence.begin() + static_cast<std::ptrdiff_t>(end));
            inJob += end - start;
        }
    }

    std::map<std::string, ResultPicture> results;
    const uint32_t total = static_cast<uint32_t>(sources.size());
    uint32_t finished = 0;
    for (const std::vector<std::vector<std::size_t>>& job : jobs) {
        JobRequest request;
        request.model = options.model;
        request.seed = options.seed;
        request.scale = 2;
        for (const std::vector<std::size_t>& sequence : job) {
            std::vector<InputPicture> clip;
            for (std::size_t index : sequence) {
                const SourcePicture& source = sources[index];
                InputPicture picture{pictureId(source), source.width, source.height,
                                     inputRgb(source), {}};
                if (source.kind == CDMDPI_PICTURE_SPRITE) {
                    for (uint8_t own : source.own) {
                        picture.alpha.push_back(own ? 255 : 0);
                    }
                }
                clip.push_back(std::move(picture));
            }
            request.sequences.push_back(std::move(clip));
        }
        const std::string id = client.submit(request);
        for (;;) {
            if (hooks.cancelled && hooks.cancelled()) {
                client.cancel(id);
                throw UpscaleError("Upscaling was cancelled.");
            }
            const JobState state = client.poll(id);
            if (hooks.progress) {
                hooks.progress(finished + state.done, total);
            }
            if (state.state == "done") {
                for (const ResultPicture& result : state.results) {
                    results[result.id] = result;
                }
                break;
            }
            if (state.state == "failed" || state.state == "cancelled") {
                throw ServiceError("the service's job " + state.state +
                                   (state.error.empty() ? "" : ": " + state.error));
            }
            std::this_thread::sleep_for(hooks.pollInterval);
        }
        for (const std::vector<std::size_t>& sequence : job) {
            finished += static_cast<uint32_t>(sequence.size());
        }
    }

    std::vector<Proposal> proposals;
    for (const SourcePicture& source : sources) {
        const auto result = results.find(pictureId(source));
        if (result == results.end()) {
            throw ServiceError("the service sent no result for " + pictureId(source));
        }
        try {
            proposals.push_back(makeProposal(source, result->second.rgb, result->second.width,
                                             result->second.height, options.colors));
        } catch (const std::invalid_argument& error) {
            throw ServiceError("the result for " + pictureId(source) + " does not fit: " +
                               error.what());
        }
    }
    return proposals;
}

}  // namespace seedvr
