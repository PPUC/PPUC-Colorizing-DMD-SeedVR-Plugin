#pragma once

// The client of the upscaling service (service/seedvr_service.py, protocol 1; the protocol is
// described in the README).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace seedvr {

constexpr int kProtocol = 1;

// The service could not be reached or refused the request; the text says why.
class ServiceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ServiceInfo {
    int protocol = 0;
    std::string backend;
    std::vector<std::string> models;
    // The most pictures one job takes.
    uint32_t maxFrames = 0;
    bool busy = false;
};

// A picture sent, RGB (3 bytes a pixel); alpha (1 byte a pixel, 255 own) optional.
struct InputPicture {
    std::string id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> alpha;
};

struct JobRequest {
    std::string model;
    int64_t seed = 42;
    uint32_t scale = 2;
    // Each sequence is upscaled as one clip (consecutive frames), in order.
    std::vector<std::vector<InputPicture>> sequences;
};

struct ResultPicture {
    std::string id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
};

struct JobState {
    // "queued", "running", "done", "failed" or "cancelled".
    std::string state;
    uint32_t done = 0;
    uint32_t total = 0;
    std::string error;
    // When done.
    std::vector<ResultPicture> results;
};

class ServiceClient {
public:
    // url: "http://host:port"; token: sent as a bearer token when not empty.
    ServiceClient(std::string url, std::string token, int timeoutSeconds = 30);

    [[nodiscard]] ServiceInfo info() const;
    // The job's ID.
    [[nodiscard]] std::string submit(const JobRequest& request) const;
    [[nodiscard]] JobState poll(const std::string& job) const;
    void cancel(const std::string& job) const;

private:
    std::string m_url;
    std::string m_token;
    int m_timeout;
};

// The request's JSON body and the answers parsed, for the tests.
[[nodiscard]] std::string requestBody(const JobRequest& request);
[[nodiscard]] JobState parseJobState(const std::string& body);
[[nodiscard]] ServiceInfo parseInfo(const std::string& body);

}  // namespace seedvr
