// Tests of the plugin without the editor: the picture logic, the gathering on a fake editor API
// table, and the client and the run against a stub service in this program.
//
// With "--service <url> [token]" it runs the client against that service instead (the Python
// service's tests start it so, with the stub backend).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "ColorizingDMDPlugin.h"
#include "Pictures.h"
#include "ServiceClient.h"
#include "Upscale.h"

namespace {

using namespace seedvr;
using nlohmann::json;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: seedvr: " << message << '\n';
        std::exit(1);
    }
}

// RGB of a picture of RGB565 colors, doubled (nearest): what the stub service gives back.
std::vector<uint8_t> doubledRgb(const std::vector<uint16_t>& pixels, uint32_t width,
                                uint32_t height)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 12);
    for (uint32_t y = 0; y < height * 2; ++y) {
        for (uint32_t x = 0; x < width * 2; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * width * 2 + x) * 3;
            fromRgb565(pixels[(y / 2) * width + x / 2], rgb[at], rgb[at + 1], rgb[at + 2]);
        }
    }
    return rgb;
}

std::vector<uint16_t> pattern(std::size_t count, uint16_t base)
{
    std::vector<uint16_t> pixels(count);
    for (std::size_t index = 0; index < count; ++index) {
        pixels[index] = static_cast<uint16_t>(base + (index % 7) * 0x0841);
    }
    return pixels;
}

void testColors()
{
    bool exact = true;
    for (uint32_t color = 0; color <= 0xffff; ++color) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        fromRgb565(static_cast<uint16_t>(color), r, g, b);
        exact = exact && toRgb565(r, g, b) == color;
    }
    expect(exact, "every RGB565 color comes back exactly from 8 bits");
    expect(toRgb565(255, 255, 255) == 0xffff && toRgb565(4, 2, 3) == 0 &&
               toRgb565(5, 0, 0) == 0x0800,
           "8 bits round to the nearest level");
    expect(colorModeFromName(colorModeName(ColorMode::Neighbourhood)) ==
                   ColorMode::Neighbourhood &&
               colorModeFromName("x") == ColorMode::Any,
           "color mode names");
    const std::vector<uint8_t> bytes{0, 1, 2, 250, 251};
    expect(base64Encode(bytes) == "AAEC+vs=" && base64Decode("AAEC+vs=") == bytes,
           "base64 both ways");
    bool refused = false;
    try {
        static_cast<void>(base64Decode("A*"));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    expect(refused, "text that is not base64 is refused");
}

SourcePicture smallSource()
{
    // 4x2, the right half not the picture's own (background).
    SourcePicture source;
    source.kind = CDMDPI_PICTURE_FRAME;
    source.index = 3;
    source.width = 4;
    source.height = 2;
    source.pixels = {0xf800, 0x07e0, 0x0001, 0x0002, 0x001f, 0xffff, 0x0003, 0x0004};
    source.own = {1, 1, 0, 0, 1, 1, 0, 0};
    source.targetWidth = 8;
    source.targetHeight = 4;
    return source;
}

void testInputAndProposals()
{
    SourcePicture source = smallSource();
    std::vector<uint8_t> rgb = inputRgb(source);
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    fromRgb565(0x07e0, r, g, b);
    expect(rgb.size() == 24 && rgb[2 * 3 + 1] == g && rgb[3 * 3 + 1] == g,
           "pixels not its own take the nearest own color");
    source.context = {0, 0, 0x1234, 0x1234, 0, 0, 0x1234, 0x1234};
    rgb = inputRgb(source);
    fromRgb565(0x1234, r, g, b);
    expect(rgb[2 * 3] == r && rgb[7 * 3 + 2] == b, "or the background when it is given");

    // The result written only where the picture is its own (write from the own pixels).
    source.write.assign(32, 0);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            source.write[y * 8 + x] = 1;
        }
    }
    std::vector<uint8_t> result = doubledRgb(source.pixels, 4, 2);
    Proposal proposal = makeProposal(source, result, 8, 4, ColorMode::Any);
    expect(proposal.kind == CDMDPI_PICTURE_FRAME && proposal.index == 3 &&
               proposal.pixels.size() == 32 && proposal.pixels[1 * 8 + 3] == 0x07e0 &&
               proposal.write[1 * 8 + 3] == 1 && proposal.write[0 * 8 + 5] == 0 &&
               proposal.pixels[0 * 8 + 5] == 0,
           "a doubled result comes back exactly, where it may be written");

    // A color between red and green: Any rounds it, the color modes take one of the source's.
    result[(0 * 8 + 1) * 3] = 200;
    result[(0 * 8 + 1) * 3 + 1] = 30;
    result[(0 * 8 + 1) * 3 + 2] = 0;
    expect(makeProposal(source, result, 8, 4, ColorMode::Any).pixels[1] == toRgb565(200, 30, 0),
           "any color: rounded");
    expect(makeProposal(source, result, 8, 4, ColorMode::Picture).pixels[1] == 0xf800 &&
               makeProposal(source, result, 8, 4, ColorMode::Neighbourhood).pixels[1] == 0xf800,
           "the picture's or the neighbours' nearest color");
    // Blue at HD (3, 0), over SD pixel (1, 0): its neighbours include 0x001f diagonally below.
    result[(0 * 8 + 3) * 3] = 0;
    result[(0 * 8 + 3) * 3 + 1] = 0;
    result[(0 * 8 + 3) * 3 + 2] = 255;
    expect(makeProposal(source, result, 8, 4, ColorMode::Neighbourhood).pixels[3] == 0x001f,
           "the neighbours include the diagonal ones");

    bool refused = false;
    try {
        static_cast<void>(makeProposal(source, std::vector<uint8_t>(10), 3, 1, ColorMode::Any));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    expect(refused, "a result of another size is refused");
}

// ---- A fake editor -----------------------------------------------------------------------
// Three frames of 128x32 with an HD plane; frame 1 shows background 0 on its right half and
// has no HD yet; frame 2 has HD and dynamic zones. Sprite 0 is a 3x2 block at (10, 4).

struct FakeProject {
    bool hd = true;
    std::vector<std::vector<uint16_t>> frames;
    std::vector<uint16_t> background;
};
FakeProject g_fake;

template <typename T>
uint32_t give(const std::vector<T>& values, T* buffer, uint32_t capacity)
{
    if (buffer && capacity >= values.size()) {
        std::copy(values.begin(), values.end(), buffer);
    }
    return static_cast<uint32_t>(values.size());
}

std::vector<uint8_t> rightHalf(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> mask(static_cast<std::size_t>(width) * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = width / 2; x < width; ++x) {
            mask[y * width + x] = 1;
        }
    }
    return mask;
}

std::vector<uint8_t> spriteShape(bool hd)
{
    std::vector<uint8_t> shape(CDMDPI_SPRITE_WIDTH * CDMDPI_SPRITE_HEIGHT, 0);
    const uint32_t scale = hd ? 2 : 1;
    for (uint32_t y = 4 * scale; y < 6 * scale; ++y) {
        for (uint32_t x = 10 * scale; x < 13 * scale; ++x) {
            shape[y * CDMDPI_SPRITE_WIDTH + x] = 1;
        }
    }
    return shape;
}

ColorizingDMDEditorAPI fakeApi()
{
    ColorizingDMDEditorAPI api{};
    api.version = 3;
    api.GetProjectInfo = [](ColorizingDMDProjectInfo* info) -> int {
        *info = ColorizingDMDProjectInfo{};
        info->hasProject = 1;
        info->frameCount = static_cast<uint32_t>(g_fake.frames.size());
        info->sdWidth = 128;
        info->sdHeight = 32;
        info->hdWidth = g_fake.hd ? 256 : 0;
        info->hdHeight = g_fake.hd ? 64 : 0;
        info->backgroundCount = 1;
        info->spriteCount = 1;
        return 1;
    };
    api.GetFrameInfo = [](uint32_t frame, ColorizingDMDFrameInfo* info) -> int {
        if (frame >= g_fake.frames.size()) {
            return 0;
        }
        *info = ColorizingDMDFrameInfo{};
        info->background = frame == 1 ? 0 : CDMDPI_NONE;
        info->hasHd = frame == 2 ? 1 : 0;
        info->hasDynamicPixels = frame == 2 ? 1 : 0;
        return 1;
    };
    api.GetColorizedFrame = [](uint32_t frame, int hd, uint16_t* pixels, uint32_t capacity,
                               uint32_t*, uint32_t*) -> uint32_t {
        return frame < g_fake.frames.size() && !hd ? give(g_fake.frames[frame], pixels, capacity)
                                                   : 0;
    };
    api.GetBackgroundMask = [](uint32_t frame, int hd, uint8_t* pixels, uint32_t capacity,
                               uint32_t*, uint32_t*) -> uint32_t {
        const uint32_t width = hd ? 256 : 128;
        const uint32_t height = hd ? 64 : 32;
        return give(frame == 1 ? rightHalf(width, height)
                               : std::vector<uint8_t>(static_cast<std::size_t>(width) * height),
                    pixels, capacity);
    };
    api.GetBackgroundPicture = [](uint32_t background, int hd, uint16_t* pixels,
                                  uint32_t capacity, uint32_t*, uint32_t*) -> uint32_t {
        return background == 0 && !hd ? give(g_fake.background, pixels, capacity) : 0;
    };
    api.GetDynamicMask = [](uint32_t, uint8_t* pixels, uint32_t capacity, uint32_t*,
                            uint32_t*) -> uint32_t {
        // SD dynamic plane: pixel (0, 0) is in set 0.
        std::vector<uint8_t> zones(128 * 32, 255);
        zones[0] = 0;
        return give(zones, pixels, capacity);
    };
    api.GetFrameSprites = [](uint32_t frame, uint32_t* sprites, uint32_t capacity) -> uint32_t {
        return give(frame == 1 ? std::vector<uint32_t>{0} : std::vector<uint32_t>{}, sprites,
                    capacity);
    };
    api.GetSpritePicture = [](uint32_t sprite, int hd, uint16_t* pixels, uint32_t capacity,
                              uint32_t*, uint32_t*) -> uint32_t {
        return sprite == 0 && !hd
                   ? give(pattern(CDMDPI_SPRITE_WIDTH * CDMDPI_SPRITE_HEIGHT, 0x2000), pixels,
                          capacity)
                   : 0;
    };
    api.GetSpriteMask = [](uint32_t sprite, int hd, uint8_t* pixels, uint32_t capacity,
                           uint32_t*, uint32_t*) -> uint32_t {
        // No HD sprite yet.
        return sprite == 0 && !hd ? give(spriteShape(false), pixels, capacity) : 0;
    };
    return api;
}

void testGathering()
{
    g_fake.frames = {pattern(128 * 32, 0x1000), pattern(128 * 32, 0x3000),
                     pattern(128 * 32, 0x5000)};
    g_fake.background = std::vector<uint16_t>(128 * 32, 0x0f0f);
    const ColorizingDMDEditorAPI api = fakeApi();
    Options options;
    std::vector<SourcePicture> sources = gatherFrames(&api, {0, 1, 2, 7}, options);
    expect(sources.size() == 3 && sources[0].own.empty() && sources[0].write.empty() &&
               sources[0].targetWidth == 256,
           "a frame without background: all its own, written everywhere");
    const SourcePicture& withBackground = sources[1];
    expect(withBackground.own.size() == 128 * 32 && withBackground.own[5] == 1 &&
               withBackground.own[100] == 0 && withBackground.context.size() == 128 * 32 &&
               withBackground.context[100] == 0x0f0f,
           "a frame with background: its own pixels, the background as context");
    expect(withBackground.write.size() == 256 * 64 && withBackground.write[10] == 1 &&
               withBackground.write[200] == 0 && withBackground.write[63 * 256 + 255] == 0,
           "written where its doubled mask shows the frame");
    options.backgroundInInput = false;
    options.skipDynamicZones = true;
    sources = gatherFrames(&api, {1, 2}, options);
    expect(sources[0].context.empty(), "without the background in the input");
    expect(sources[1].write.size() == 256 * 64 && sources[1].write[0] == 0 &&
               sources[1].write[1] == 0 && sources[1].write[256] == 0 &&
               sources[1].write[2] == 1,
           "dynamic zones left out, doubled from the SD dynamic plane");

    const std::vector<SourcePicture> backgrounds =
        gatherBackgrounds(&api, backgroundsOf(&api, {0, 1, 2}));
    expect(backgrounds.size() == 1 && backgrounds[0].kind == CDMDPI_PICTURE_BACKGROUND &&
               backgrounds[0].pixels[0] == 0x0f0f && backgrounds[0].write.empty(),
           "the frames' background");

    const std::vector<SourcePicture> sprites = gatherSprites(&api, spritesOf(&api, {0, 1, 2}));
    expect(sprites.size() == 1 && sprites[0].kind == CDMDPI_PICTURE_SPRITE &&
               sprites[0].width == 3 && sprites[0].height == 2 && sprites[0].offsetX == 20 &&
               sprites[0].offsetY == 8 &&
               sprites[0].pixels[0] == pattern(256 * 64, 0x2000)[4 * 256 + 10],
           "the frames' sprite, the part it covers");
    expect(sprites[0].write.size() == 256 * 64 && sprites[0].write[8 * 256 + 20] == 1 &&
               sprites[0].write[11 * 256 + 25] == 1 && sprites[0].write[12 * 256 + 20] == 0,
           "written where the doubled sprite covers");

    g_fake.hd = false;
    bool refused = false;
    try {
        static_cast<void>(gatherFrames(&api, {0}, options));
    } catch (const UpscaleError&) {
        refused = true;
    }
    expect(refused, "a project without HD is refused");
    g_fake.hd = true;

    SourcePicture frame;
    frame.kind = CDMDPI_PICTURE_FRAME;
    std::vector<SourcePicture> list;
    for (uint32_t index : {4u, 5u, 6u, 9u, 10u}) {
        frame.index = index;
        list.push_back(frame);
    }
    SourcePicture background;
    background.kind = CDMDPI_PICTURE_BACKGROUND;
    background.index = 11;
    list.push_back(background);
    const auto sequences = sequencesOf(list, 2);
    expect(sequences.size() == 4 && sequences[0] == std::vector<std::size_t>{0, 1} &&
               sequences[1] == std::vector<std::size_t>{2} &&
               sequences[2] == std::vector<std::size_t>{3, 4} &&
               sequences[3] == std::vector<std::size_t>{5} && pictureId(list[5]) == "b11",
           "clips of consecutive frames, at most the run length; a background alone");
}

// ---- A stub service in this program ------------------------------------------------------

class StubService {
public:
    explicit StubService(uint32_t maxFrames)
        : m_maxFrames(maxFrames)
    {
        m_server.Get("/v1/info", [this](const httplib::Request&, httplib::Response& response) {
            response.set_content(json{{"protocol", 1},
                                      {"backend", "stub"},
                                      {"models", {"3b"}},
                                      {"maxFrames", m_maxFrames}}
                                     .dump(),
                                 "application/json");
        });
        m_server.Post("/v1/jobs", [this](const httplib::Request& request,
                                         httplib::Response& response) {
            const json body = json::parse(request.body);
            json results = json::array();
            std::size_t count = 0;
            for (const json& sequence : body["sequences"]) {
                ++m_clips;
                for (const json& frame : sequence["frames"]) {
                    ++count;
                    const uint32_t width = frame["width"];
                    const uint32_t height = frame["height"];
                    const std::vector<uint8_t> rgb = base64Decode(frame["rgb"]);
                    std::vector<uint8_t> doubled(rgb.size() * 4);
                    for (uint32_t y = 0; y < height * 2; ++y) {
                        for (uint32_t x = 0; x < width * 2; ++x) {
                            for (int c = 0; c < 3; ++c) {
                                doubled[(y * width * 2 + x) * 3 + c] =
                                    rgb[((y / 2) * width + x / 2) * 3 + c];
                            }
                        }
                    }
                    results.push_back({{"id", frame["id"]},
                                       {"width", width * 2},
                                       {"height", height * 2},
                                       {"rgb", base64Encode(doubled)}});
                }
            }
            if (count > m_maxFrames) {
                response.status = 400;
                response.set_content(R"({"error":"too many frames"})", "application/json");
                return;
            }
            const std::lock_guard<std::mutex> lock(m_mutex);
            const std::string id = "job" + std::to_string(m_jobs.size());
            m_jobs[id] = json{{"state", m_fail ? "failed" : "done"},
                              {"done", count},
                              {"total", count},
                              {"results", results}};
            if (m_fail) {
                m_jobs[id]["error"] = "out of memory";
            }
            response.status = 202;
            response.set_content(json{{"job", id}}.dump(), "application/json");
        });
        m_server.Get(R"(/v1/jobs/(\w+))", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto job = m_jobs.find(request.matches[1]);
            if (job == m_jobs.end()) {
                response.status = 404;
                return;
            }
            // The first poll finds it running.
            json answer = job->second;
            if (m_polled.insert(job->first).second) {
                answer = json{{"state", "running"}, {"done", 0}, {"total", answer["total"]}};
            }
            response.set_content(answer.dump(), "application/json");
        });
        m_server.Delete(R"(/v1/jobs/(\w+))", [this](const httplib::Request&,
                                                    httplib::Response& response) {
            ++m_cancels;
            response.set_content("{}", "application/json");
        });
        m_port = m_server.bind_to_any_port("127.0.0.1");
        m_thread = std::thread([this]() { m_server.listen_after_bind(); });
        m_server.wait_until_ready();
    }
    ~StubService()
    {
        m_server.stop();
        m_thread.join();
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(m_port); }
    [[nodiscard]] int jobs() const { return static_cast<int>(m_jobs.size()); }
    [[nodiscard]] int clips() const { return m_clips; }
    [[nodiscard]] int cancels() const { return m_cancels; }
    void setFail(bool fail) { m_fail = fail; }

private:
    httplib::Server m_server;
    std::thread m_thread;
    int m_port = 0;
    uint32_t m_maxFrames;
    std::mutex m_mutex;
    std::map<std::string, json> m_jobs;
    std::set<std::string> m_polled;
    std::atomic<int> m_clips{0};
    std::atomic<int> m_cancels{0};
    bool m_fail = false;
};

std::vector<SourcePicture> framesToUpscale(uint32_t count)
{
    std::vector<SourcePicture> sources;
    for (uint32_t index = 0; index < count; ++index) {
        SourcePicture source;
        source.kind = CDMDPI_PICTURE_FRAME;
        source.index = 10 + index;
        source.width = 128;
        source.height = 32;
        source.pixels = pattern(128 * 32, static_cast<uint16_t>(0x0800 * (index + 1)));
        source.targetWidth = 256;
        source.targetHeight = 64;
        sources.push_back(std::move(source));
    }
    return sources;
}

// The run gives each frame back doubled, exactly.
void checkDoubled(const std::vector<SourcePicture>& sources,
                  const std::vector<Proposal>& proposals, const std::string& what)
{
    bool exact = proposals.size() == sources.size();
    for (std::size_t index = 0; exact && index < proposals.size(); ++index) {
        const Proposal& proposal = proposals[index];
        exact = proposal.index == sources[index].index && proposal.pixels.size() == 256 * 64;
        for (uint32_t y = 0; exact && y < 64; ++y) {
            for (uint32_t x = 0; exact && x < 256; ++x) {
                exact = proposal.pixels[y * 256 + x] ==
                            sources[index].pixels[(y / 2) * 128 + x / 2] &&
                        proposal.write[y * 256 + x] == 1;
            }
        }
    }
    expect(exact, what);
}

void testRun()
{
    StubService service(5);
    const ServiceClient client(service.url(), "");
    const ServiceInfo info = client.info();
    expect(info.protocol == 1 && info.backend == "stub" && info.maxFrames == 5,
           "the service's info");

    RunHooks hooks;
    hooks.pollInterval = std::chrono::milliseconds(1);
    uint32_t lastTotal = 0;
    hooks.progress = [&lastTotal](uint32_t, uint32_t total) { lastTotal = total; };
    Options options;
    options.runLength = 3;
    const std::vector<SourcePicture> sources = framesToUpscale(7);
    checkDoubled(sources, runUpscale(client, sources, options, hooks),
                 "the frames come back doubled, exactly");
    // 7 frames in clips of 3: 3, 3, 1; at most 5 a job: (3), (3, 1).
    expect(service.jobs() == 2 && service.clips() == 3 && lastTotal == 7,
           "in jobs of whole clips the service takes");

    hooks.cancelled = []() { return true; };
    bool cancelled = false;
    try {
        static_cast<void>(runUpscale(client, sources, options, hooks));
    } catch (const UpscaleError&) {
        cancelled = true;
    }
    expect(cancelled && service.cancels() == 1, "a cancelled run cancels the job");

    hooks.cancelled = nullptr;
    service.setFail(true);
    bool failed = false;
    try {
        static_cast<void>(runUpscale(client, framesToUpscale(1), options, hooks));
    } catch (const ServiceError& error) {
        failed = std::string(error.what()).find("out of memory") != std::string::npos;
    }
    expect(failed, "a failed job fails the run with the service's reason");

    bool unreachable = false;
    try {
        static_cast<void>(ServiceClient("http://127.0.0.1:1", "", 2).info());
    } catch (const ServiceError&) {
        unreachable = true;
    }
    expect(unreachable, "a service that does not answer");
}

// Against a running service (stub backend), as the plugin talks to it.
int runAgainst(const std::string& url, const std::string& token)
{
    const ServiceClient client(url, token);
    const ServiceInfo info = client.info();
    expect(info.protocol == 1 && info.backend == "stub", "the service's info");
    RunHooks hooks;
    hooks.pollInterval = std::chrono::milliseconds(10);
    Options options;
    options.runLength = 2;
    std::vector<SourcePicture> sources = framesToUpscale(5);
    checkDoubled(sources, runUpscale(client, sources, options, hooks),
                 "the service gives the frames back doubled, exactly");

    // A sprite (with alpha) placed into its target.
    SourcePicture sprite;
    sprite.kind = CDMDPI_PICTURE_SPRITE;
    sprite.index = 0;
    sprite.width = 3;
    sprite.height = 2;
    sprite.pixels = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0841, 0x1082};
    sprite.own = {1, 1, 1, 1, 0, 1};
    sprite.targetWidth = 256;
    sprite.targetHeight = 64;
    sprite.offsetX = 20;
    sprite.offsetY = 8;
    const std::vector<Proposal> proposals = runUpscale(client, {sprite}, options, hooks);
    expect(proposals.size() == 1 && proposals[0].pixels[8 * 256 + 20] == 0xf800 &&
               proposals[0].pixels[11 * 256 + 25] == 0x1082 && proposals[0].write[0] == 0,
           "a sprite comes back where it lies in the HD sprite");
    std::cout << "seedvr client against " << url << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--service") {
        return runAgainst(argv[2], argc >= 4 ? argv[3] : "");
    }
    testColors();
    testInputAndProposals();
    testGathering();
    testRun();
    std::cout << "seedvr_tests passed\n";
    return 0;
}
