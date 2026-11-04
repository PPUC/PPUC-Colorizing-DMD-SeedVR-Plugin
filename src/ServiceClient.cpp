#include "ServiceClient.h"

#include <utility>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "Pictures.h"

namespace seedvr {

namespace {

using nlohmann::json;

std::string describe(const httplib::Result& result)
{
    if (!result) {
        return "no answer (" + httplib::to_string(result.error()) + ")";
    }
    std::string text = "HTTP " + std::to_string(result->status);
    try {
        const json body = json::parse(result->body);
        if (body.contains("error")) {
            text += ": " + body["error"].get<std::string>();
        }
    } catch (const std::exception&) {
        // Not JSON: the status says enough.
    }
    return text;
}

json parseBody(const std::string& body)
{
    try {
        return json::parse(body);
    } catch (const std::exception& error) {
        throw ServiceError(std::string("the service's answer is not JSON: ") + error.what());
    }
}

}  // namespace

ServiceClient::ServiceClient(std::string url, std::string token, int timeoutSeconds)
    : m_url(std::move(url))
    , m_token(std::move(token))
    , m_timeout(timeoutSeconds)
{
    while (!m_url.empty() && m_url.back() == '/') {
        m_url.pop_back();
    }
}

std::string requestBody(const JobRequest& request)
{
    json sequences = json::array();
    for (const std::vector<InputPicture>& sequence : request.sequences) {
        json frames = json::array();
        for (const InputPicture& picture : sequence) {
            json frame = {{"id", picture.id},
                          {"width", picture.width},
                          {"height", picture.height},
                          {"rgb", base64Encode(picture.rgb)}};
            if (!picture.alpha.empty()) {
                frame["alpha"] = base64Encode(picture.alpha);
            }
            frames.push_back(std::move(frame));
        }
        sequences.push_back({{"frames", std::move(frames)}});
    }
    return json{{"protocol", kProtocol},
                {"model", request.model},
                {"seed", request.seed},
                {"scale", request.scale},
                {"sequences", std::move(sequences)}}
        .dump();
}

ServiceInfo parseInfo(const std::string& body)
{
    const json answer = parseBody(body);
    try {
        ServiceInfo info;
        info.protocol = answer.value("protocol", 0);
        info.backend = answer.value("backend", std::string());
        info.models = answer.value("models", std::vector<std::string>());
        info.maxFrames = answer.value("maxFrames", 0u);
        info.busy = answer.value("busy", false);
        return info;
    } catch (const std::exception& error) {
        throw ServiceError(std::string("the service's info is not as expected: ") + error.what());
    }
}

JobState parseJobState(const std::string& body)
{
    const json answer = parseBody(body);
    try {
        JobState state;
        state.state = answer.at("state").get<std::string>();
        state.done = answer.value("done", 0u);
        state.total = answer.value("total", 0u);
        state.error = answer.value("error", std::string());
        if (answer.contains("results")) {
            for (const json& item : answer["results"]) {
                ResultPicture result;
                result.id = item.at("id").get<std::string>();
                result.width = item.at("width").get<uint32_t>();
                result.height = item.at("height").get<uint32_t>();
                result.rgb = base64Decode(item.at("rgb").get<std::string>());
                if (result.rgb.size() != static_cast<std::size_t>(result.width) *
                                             result.height * 3) {
                    throw std::invalid_argument("result " + result.id +
                                                " has not its size in RGB");
                }
                state.results.push_back(std::move(result));
            }
        }
        return state;
    } catch (const ServiceError&) {
        throw;
    } catch (const std::exception& error) {
        throw ServiceError(std::string("the service's job state is not as expected: ") +
                           error.what());
    }
}

ServiceInfo ServiceClient::info() const
{
    httplib::Client client(m_url);
    client.set_connection_timeout(m_timeout);
    client.set_read_timeout(m_timeout);
    if (!m_token.empty()) {
        client.set_bearer_token_auth(m_token);
    }
    const httplib::Result result = client.Get("/v1/info");
    if (!result || result->status != 200) {
        throw ServiceError("the service at " + m_url + " does not answer: " + describe(result));
    }
    ServiceInfo info = parseInfo(result->body);
    if (info.protocol != kProtocol) {
        throw ServiceError("the service at " + m_url + " speaks protocol " +
                           std::to_string(info.protocol) + ", the plugin " +
                           std::to_string(kProtocol));
    }
    return info;
}

std::string ServiceClient::submit(const JobRequest& request) const
{
    httplib::Client client(m_url);
    client.set_connection_timeout(m_timeout);
    client.set_read_timeout(m_timeout);
    client.set_write_timeout(m_timeout);
    if (!m_token.empty()) {
        client.set_bearer_token_auth(m_token);
    }
    const httplib::Result result =
        client.Post("/v1/jobs", requestBody(request), "application/json");
    if (!result || (result->status != 200 && result->status != 202)) {
        throw ServiceError("the service refused the job: " + describe(result));
    }
    const json answer = parseBody(result->body);
    if (!answer.contains("job") || !answer["job"].is_string()) {
        throw ServiceError("the service named no job");
    }
    return answer["job"].get<std::string>();
}

JobState ServiceClient::poll(const std::string& job) const
{
    httplib::Client client(m_url);
    client.set_connection_timeout(m_timeout);
    client.set_read_timeout(m_timeout);
    if (!m_token.empty()) {
        client.set_bearer_token_auth(m_token);
    }
    const httplib::Result result = client.Get("/v1/jobs/" + job);
    if (!result || result->status != 200) {
        throw ServiceError("the service lost the job: " + describe(result));
    }
    return parseJobState(result->body);
}

void ServiceClient::cancel(const std::string& job) const
{
    httplib::Client client(m_url);
    client.set_connection_timeout(m_timeout);
    client.set_read_timeout(m_timeout);
    if (!m_token.empty()) {
        client.set_bearer_token_auth(m_token);
    }
    static_cast<void>(client.Delete("/v1/jobs/" + job));
}

}  // namespace seedvr
