#ifndef APPLESS_LITE_SERVER_REVERSE_CLIENT_H
#define APPLESS_LITE_SERVER_REVERSE_CLIENT_H

#include "lite_llm.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace appless::lite_server {

struct ReverseClientOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string auth_token;
    std::size_t max_frame_bytes = 8U * 1024U * 1024U;
    std::uint32_t reconnect_initial_ms = 250;
    std::uint32_t reconnect_max_ms = 5000;
};

class ReverseClient final {
public:
    ReverseClient(ReverseClientOptions options, lite_llm::LiteLlm &llm,
                  std::function<bool()> should_stop);

    int Run();

private:
    struct SessionState;

    int Connect(std::string *error) const;
    bool RunSession(int socket_fd, std::string *error);
    bool PerformHandshake(int socket_fd, std::string *error);
    bool HandleMessage(const std::shared_ptr<SessionState> &session,
                       const std::string &payload, std::string *error);
    bool SendFrame(int socket_fd, const std::string &payload, std::string *error) const;
    bool SendFrame(const std::shared_ptr<SessionState> &session,
                   const std::string &payload, std::string *error) const;
    bool ReceiveFrame(int socket_fd, int timeout_ms, std::string *payload, std::string *error) const;
    void CloseSession(const std::shared_ptr<SessionState> &session) const;
    void SleepForReconnect(std::uint32_t milliseconds) const;

    ReverseClientOptions options_;
    lite_llm::LiteLlm &llm_;
    std::function<bool()> should_stop_;
};

}  // namespace appless::lite_server

#endif
