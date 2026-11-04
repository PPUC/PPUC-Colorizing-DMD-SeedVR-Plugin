// The plugin's entry points: loaded by the ColorizingDMD editor from its plugin folder (plugin
// id "SeedVR", see plugin.cfg). The menu entries gather the pictures on the editor's main
// thread; a worker thread runs them through the service and hands the results to the editor's
// review window.

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/MsgPlugin.h"

#include "Upscale.h"

namespace {

const char* const kName = "SeedVR";
const char* const kDefaultUrl = "http://127.0.0.1:8976";

const MsgPluginAPI* g_msgApi = nullptr;
uint32_t g_endpoint = 0;
const ColorizingDMDEditorAPI* g_editorApi = nullptr;
LoggingPluginAPI* g_logging = nullptr;

// The run in progress, if any.
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};

// Calls waiting for the main thread, and whether the plugin is unloading (see unload).
std::mutex g_jobsMutex;
int g_jobsInFlight = 0;
bool g_stopping = false;

void log(unsigned int level, const std::string& message)
{
    if (g_logging) {
        g_logging->Log(kName, "", 0, level, message.c_str());
    }
}

struct Job {
    const std::function<void()>* work;
    std::exception_ptr error;
};

void MSGPIAPI runJob(void* userData)
{
    Job* job = static_cast<Job*>(userData);
    try {
        (*job->work)();
    } catch (...) {
        job->error = std::current_exception();
    }
}

// On the worker thread: runs the work on the editor's main thread and waits for it.
void runOnMainThread(const std::function<void()>& work)
{
    {
        const std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (g_stopping) {
            throw std::runtime_error("the plugin is unloading");
        }
        ++g_jobsInFlight;
    }
    Job job{&work, nullptr};
    g_msgApi->RunOnMainThread(g_endpoint, -1.0, &runJob, &job);
    {
        const std::lock_guard<std::mutex> lock(g_jobsMutex);
        --g_jobsInFlight;
    }
    if (job.error) {
        std::rethrow_exception(job.error);
    }
}

std::string setting(const char* key, const std::string& fallback = {})
{
    const uint32_t size = g_editorApi->GetSetting(g_endpoint, key, nullptr, 0);
    if (size == 0) {
        return fallback;
    }
    std::string value(size, '\0');
    g_editorApi->GetSetting(g_endpoint, key, value.data(), size);
    value.resize(size - 1);
    return value.empty() ? fallback : value;
}

long long numberSetting(const char* key, long long fallback)
{
    try {
        return std::stoll(setting(key, std::to_string(fallback)));
    } catch (const std::exception&) {
        return fallback;
    }
}

seedvr::Options options()
{
    seedvr::Options result;
    result.model = setting("model", result.model);
    result.seed = numberSetting("seed", result.seed);
    result.colors = seedvr::colorModeFromName(
        setting("colors", seedvr::colorModeName(result.colors)));
    result.backgroundInInput = setting("background", "1") == "1";
    result.skipDynamicZones = setting("dynamic", "0") == "1";
    const long long run = numberSetting("run", result.runLength);
    result.runLength = static_cast<uint32_t>(run < 1 ? 1 : (run > 256 ? 256 : run));
    return result;
}

void showMessage(const std::string& text)
{
    g_editorApi->ShowMessage(g_endpoint, kName, text.c_str());
}

void showStatus(const std::string& text)
{
    g_editorApi->ShowStatus(g_endpoint, text.c_str());
}

void MSGPIAPI onReviewed(void*, uint32_t applied)
{
    if (g_editorApi) {
        showStatus(applied > 0 ? "SeedVR: " + std::to_string(applied) + " picture(s) applied"
                               : "SeedVR: nothing applied");
    }
}

// On the worker thread.
void run(std::vector<seedvr::SourcePicture> sources, seedvr::Options settings, std::string url,
         std::string token)
{
    try {
        const seedvr::ServiceClient client(url, token);
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        seedvr::RunHooks hooks;
        hooks.cancelled = []() { return g_cancel.load(); };
        hooks.progress = [&last](uint32_t done, uint32_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(1)) {
                return;
            }
            last = now;
            const std::string text = "SeedVR: " + std::to_string(done) + " of " +
                                     std::to_string(total) + " pictures upscaled";
            runOnMainThread([&text]() { showStatus(text); });
        };
        const std::vector<seedvr::Proposal> proposals =
            seedvr::runUpscale(client, sources, settings, hooks);
        runOnMainThread([&proposals]() {
            std::vector<ColorizingDMDPicture> pictures;
            for (const seedvr::Proposal& proposal : proposals) {
                pictures.push_back(ColorizingDMDPicture{
                    proposal.kind, proposal.index, 1, proposal.pixels.data(),
                    proposal.write.data(), static_cast<uint32_t>(proposal.pixels.size()),
                    proposal.label.c_str()});
            }
            if (!g_editorApi->ReviewPictures(g_endpoint, "SeedVR upscaling", "Upscale with SeedVR",
                                             pictures.data(),
                                             static_cast<uint32_t>(pictures.size()),
                                             &onReviewed, nullptr)) {
                showMessage("The editor did not take the upscaled pictures: the project was "
                            "closed, or a review of the upscaler is still open.");
            }
        });
    } catch (const std::exception& error) {
        const std::string text = error.what();
        log(LPI_LVL_ERROR, text);
        try {
            runOnMainThread([&text]() { showMessage(text); });
        } catch (const std::exception&) {
            // Unloading: nobody to tell.
        }
    }
    g_running = false;
}

// On the main thread: what to upscale, then the run on the worker thread.
void start(const std::function<std::vector<seedvr::SourcePicture>(const seedvr::Options&)>& gather)
{
    if (g_running) {
        showMessage("An upscaling run is still going on; cancel it first "
                    "(Plugins -> SeedVR: Cancel Upscaling).");
        return;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    const seedvr::Options settings = options();
    std::vector<seedvr::SourcePicture> sources;
    try {
        sources = gather(settings);
    } catch (const seedvr::UpscaleError& error) {
        showMessage(error.what());
        return;
    }
    if (sources.empty()) {
        showMessage("There is nothing to upscale.");
        return;
    }
    g_cancel = false;
    g_running = true;
    showStatus("SeedVR: sending " + std::to_string(sources.size()) + " pictures");
    g_worker = std::thread(&run, std::move(sources), settings, setting("url", kDefaultUrl),
                           setting("token"));
}

std::vector<uint32_t> selectedFrames()
{
    std::vector<uint32_t> frames(g_editorApi->GetSelectedFrames(nullptr, 0));
    if (!frames.empty()) {
        g_editorApi->GetSelectedFrames(frames.data(), static_cast<uint32_t>(frames.size()));
    }
    return frames;
}

void MSGPIAPI onUpscaleFrames(void*)
{
    start([](const seedvr::Options& settings) {
        return seedvr::gatherFrames(g_editorApi, selectedFrames(), settings);
    });
}

void MSGPIAPI onUpscaleBackgrounds(void*)
{
    start([](const seedvr::Options&) {
        return seedvr::gatherBackgrounds(g_editorApi,
                                         seedvr::backgroundsOf(g_editorApi, selectedFrames()));
    });
}

void MSGPIAPI onUpscaleSprites(void*)
{
    start([](const seedvr::Options&) {
        return seedvr::gatherSprites(g_editorApi,
                                     seedvr::spritesOf(g_editorApi, selectedFrames()));
    });
}

void MSGPIAPI onUpscaleCurrentBackground(void*)
{
    start([](const seedvr::Options&) {
        const uint32_t background = g_editorApi->GetCurrentBackground();
        if (background == CDMDPI_NONE) {
            throw seedvr::UpscaleError("No background is chosen in the background panel.");
        }
        return seedvr::gatherBackgrounds(g_editorApi, {background});
    });
}

void MSGPIAPI onUpscaleCurrentSprite(void*)
{
    start([](const seedvr::Options&) {
        const uint32_t sprite = g_editorApi->GetCurrentSprite();
        if (sprite == CDMDPI_NONE) {
            throw seedvr::UpscaleError("No sprite is chosen in the sprite panel.");
        }
        return seedvr::gatherSprites(g_editorApi, {sprite});
    });
}

void MSGPIAPI onCancel(void*)
{
    if (g_running) {
        g_cancel = true;
        showStatus("SeedVR: cancelling");
    } else {
        showStatus("SeedVR: no upscaling run is going on");
    }
}

void MSGPIAPI onSettingsSaved(void*, int saved)
{
    if (saved && g_editorApi) {
        showStatus("SeedVR: settings saved");
    }
}

void MSGPIAPI onSettings(void*)
{
    static const ColorizingDMDSettingField kFields[] = {
        {"url", "Service address", CDMDPI_FIELD_TEXT, kDefaultUrl, nullptr,
         "Where seedvr_service.py runs; for another machine, through an SSH tunnel or with "
         "the service's token."},
        {"token", "Token", CDMDPI_FIELD_PASSWORD, nullptr, nullptr,
         "The service's --token, if it has one."},
        {"model", "Model", CDMDPI_FIELD_TEXT, "3b", nullptr,
         "One of the models the service lists (3b, 7b)."},
        {"seed", "Seed", CDMDPI_FIELD_TEXT, "42", nullptr,
         "The same seed gives the same result for the same pictures."},
        {"colors", "Colors", CDMDPI_FIELD_CHOICE, "neighbourhood",
         "neighbourhood=Colors of the pixels around\npicture=Colors of the picture\n"
         "any=Any color",
         "Which colors an upscaled pixel may take: the colors of the source pixels around it "
         "keep edges crisp and invent none."},
        {"background", "Send frames with their background", CDMDPI_FIELD_CHECKBOX, "1", nullptr,
         "Where the background shows through, the upscaler sees it; the result is written only "
         "where the frame shows its own pixels."},
        {"dynamic", "Leave dynamic zones as they are", CDMDPI_FIELD_CHECKBOX, "0", nullptr,
         nullptr},
        {"run", "Frames per clip", CDMDPI_FIELD_TEXT, "16", nullptr,
         "Consecutive frames are upscaled together, as a video, for results that stay steady "
         "from frame to frame."},
    };
    if (!g_editorApi->EditSettings(g_endpoint, "SeedVR settings", kFields,
                                   sizeof(kFields) / sizeof(kFields[0]), &onSettingsSaved,
                                   nullptr)) {
        showStatus("SeedVR: the settings are open already");
    }
}

}  // namespace

MSGPI_EXPORT void MSGPIAPI SeedVRPluginLoad(const uint32_t endpointId, const MsgPluginAPI* api)
{
    g_msgApi = api;
    g_endpoint = endpointId;
    g_stopping = false;

    const unsigned int getLogging = api->GetMsgID(LOGPI_NAMESPACE, LOGPI_MSG_GET_API);
    api->BroadcastMsg(endpointId, getLogging, &g_logging);
    api->ReleaseMsgID(getLogging);

    const unsigned int getEditor = api->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_MSG_GET_API);
    api->BroadcastMsg(endpointId, getEditor, &g_editorApi);
    api->ReleaseMsgID(getEditor);
    if (!g_editorApi || g_editorApi->version < 3) {
        log(LPI_LVL_ERROR, "Needs the ColorizingDMD editor with editor API version 3 or later; "
                           "nothing to do.");
        g_editorApi = nullptr;
        return;
    }

    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Upscale Selected Frames...", &onUpscaleFrames,
                              nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Upscale Backgrounds of Selected Frames...",
                              &onUpscaleBackgrounds, nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Upscale Sprites of Selected Frames...",
                              &onUpscaleSprites, nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Upscale Current Background...",
                              &onUpscaleCurrentBackground, nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Upscale Current Sprite...",
                              &onUpscaleCurrentSprite, nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Cancel Upscaling", &onCancel, nullptr);
    g_editorApi->AddMenuEntry(endpointId, "SeedVR: Settings...", &onSettings, nullptr);
    log(LPI_LVL_INFO, "Upscaling through the service at " + setting("url", kDefaultUrl));
}

MSGPI_EXPORT void MSGPIAPI SeedVRPluginUnload()
{
    if (g_editorApi) {
        g_editorApi->RemoveMenuEntries(g_endpoint);
    }
    {
        const std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_stopping = true;
    }
    g_cancel = true;
    // The worker may wait for the main thread, this one: run what is queued until it ends.
    while (g_running) {
        g_msgApi->FlushPendingCallbacks(g_endpoint);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_editorApi = nullptr;
    g_logging = nullptr;
    g_msgApi = nullptr;
}
