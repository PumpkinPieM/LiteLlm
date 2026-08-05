#include "reverse_client.h"

#include "json_value.h"
#include "protocol_codec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace appless::lite_server {
namespace {

constexpr int kPollIntervalMs = 500;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeTimeoutMs = 10000;

std::string ErrnoMessage(const std::string &operation)
{
    return operation + " failed: " + std::strerror(errno) + " (errno=" + std::to_string(errno) + ")";
}

void ShutdownAndClose(int socket_fd)
{
    if (socket_fd < 0) {
        return;
    }
    (void)shutdown(socket_fd, SHUT_RDWR);
    (void)close(socket_fd);
}

bool SendAll(int socket_fd, const std::uint8_t *data, std::size_t size, std::string *error)
{
    std::size_t offset = 0;
    while (offset < size) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = send(socket_fd, data + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t sent = send(socket_fd, data + offset, size - offset, 0);
#endif
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            *error = ErrnoMessage("send");
            return false;
        }
    }
    return true;
}

}  // namespace

struct ReverseClient::SessionState {
    explicit SessionState(int descriptor) : socket_fd(descriptor) {}

    int socket_fd;
    std::mutex state_mutex;
    std::mutex send_mutex;
    std::condition_variable callbacks_finished;
    std::size_t pending_callbacks = 0;
    bool closing = false;
};

ReverseClient::ReverseClient(ReverseClientOptions options, lite_llm::LiteLlm &llm,
                             std::function<bool()> should_stop)
    : options_(std::move(options)), llm_(llm), should_stop_(std::move(should_stop))
{
}

int ReverseClient::Run()
{
    std::uint32_t reconnect_delay = options_.reconnect_initial_ms;
    while (!should_stop_()) {
        std::string error;
        const int socket_fd = Connect(&error);
        if (socket_fd < 0) {
            std::cerr << "lite-server: proxy connection failed: " << error << '\n';
            SleepForReconnect(reconnect_delay);
            reconnect_delay = std::min(options_.reconnect_max_ms, reconnect_delay * 2U);
            continue;
        }
        std::cerr << "lite-server: connected to proxy at " << options_.host << ':' << options_.port << '\n';
        const bool authenticated = RunSession(socket_fd, &error);
        ShutdownAndClose(socket_fd);
        if (should_stop_()) {
            break;
        }
        std::cerr << "lite-server: proxy session ended: " << error << '\n';
        if (authenticated) {
            reconnect_delay = options_.reconnect_initial_ms;
        }
        SleepForReconnect(reconnect_delay);
        reconnect_delay = std::min(options_.reconnect_max_ms, reconnect_delay * 2U);
    }
    return 0;
}

int ReverseClient::Connect(std::string *error) const
{
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        *error = ErrnoMessage("socket");
        return -1;
    }
    int keepalive = 1;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
        *error = "connect host must be an IPv4 address";
        ShutdownAndClose(socket_fd);
        return -1;
    }
    if (address.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        *error = "connect host must be 127.0.0.1";
        ShutdownAndClose(socket_fd);
        return -1;
    }
    const int old_flags = fcntl(socket_fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(socket_fd, F_SETFL, old_flags | O_NONBLOCK) != 0) {
        *error = ErrnoMessage("fcntl");
        ShutdownAndClose(socket_fd);
        return -1;
    }
    const int connect_result = connect(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    if (connect_result != 0 && errno != EINPROGRESS) {
        *error = ErrnoMessage("connect");
        ShutdownAndClose(socket_fd);
        return -1;
    }
    if (connect_result != 0) {
        pollfd descriptor{};
        descriptor.fd = socket_fd;
        descriptor.events = POLLOUT;
        int poll_result = 0;
        do {
            poll_result = poll(&descriptor, 1, kConnectTimeoutMs);
        } while (poll_result < 0 && errno == EINTR && !should_stop_());
        if (poll_result <= 0) {
            *error = poll_result == 0 ? "connect timed out" : ErrnoMessage("poll connect");
            ShutdownAndClose(socket_fd);
            return -1;
        }
        const short poll_error_events = descriptor.revents & (POLLERR | POLLHUP | POLLNVAL);
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0) {
            const int option_error = errno;
            const bool writable_without_poll_error =
                (descriptor.revents & POLLOUT) != 0 && poll_error_events == 0;
            if ((option_error != EACCES && option_error != EPERM) || !writable_without_poll_error) {
                errno = option_error;
                *error = ErrnoMessage("getsockopt(SO_ERROR)");
                ShutdownAndClose(socket_fd);
                return -1;
            }
        }
        if (socket_error != 0) {
            errno = socket_error;
            *error = ErrnoMessage("connect");
            ShutdownAndClose(socket_fd);
            return -1;
        }
    }
    if (fcntl(socket_fd, F_SETFL, old_flags) != 0) {
        *error = ErrnoMessage("restore socket flags");
        ShutdownAndClose(socket_fd);
        return -1;
    }
    return socket_fd;
}

bool ReverseClient::RunSession(int socket_fd, std::string *error)
{
    if (!PerformHandshake(socket_fd, error)) {
        return false;
    }
    const std::shared_ptr<SessionState> session = std::make_shared<SessionState>(socket_fd);
    std::cerr << "lite-server: proxy handshake accepted; model is ready\n";
    if (!SendFrame(session, protocol::BuildReadyStatus(), error)) {
        return true;
    }
    while (!should_stop_()) {
        std::string payload;
        if (!ReceiveFrame(socket_fd, 0, &payload, error)) {
            CloseSession(session);
            return true;
        }
        if (!HandleMessage(session, payload, error)) {
            CloseSession(session);
            return true;
        }
    }
    *error = "server is stopping";
    CloseSession(session);
    return true;
}

bool ReverseClient::PerformHandshake(int socket_fd, std::string *error)
{
    if (!SendFrame(socket_fd, protocol::BuildHello(options_.auth_token), error)) {
        return false;
    }
    std::string payload;
    if (!ReceiveFrame(socket_fd, kHandshakeTimeoutMs, &payload, error)) {
        return false;
    }
    JsonValue message;
    if (!ParseJson(payload, &message, error) || message.type() != JsonValue::Type::Object) {
        *error = "invalid hello acknowledgement: " + *error;
        return false;
    }
    const JsonValue *type = message.Find("type");
    const JsonValue *version = message.Find("protocol");
    if (type == nullptr || type->type() != JsonValue::Type::String || type->string() != "hello_ack" ||
        version == nullptr || version->type() != JsonValue::Type::Number ||
        version->number() != static_cast<double>(protocol::kProtocolVersion)) {
        *error = "proxy rejected or returned an invalid hello acknowledgement";
        return false;
    }
    return true;
}

bool ReverseClient::HandleMessage(const std::shared_ptr<SessionState> &session,
                                  const std::string &payload, std::string *error)
{
    JsonValue message;
    if (!ParseJson(payload, &message, error) || message.type() != JsonValue::Type::Object) {
        *error = "invalid proxy message: " + *error;
        return false;
    }
    const JsonValue *type = message.Find("type");
    if (type == nullptr || type->type() != JsonValue::Type::String) {
        *error = "proxy message has no valid type";
        return false;
    }
    if (type->string() == "ping") {
        return SendFrame(session, protocol::BuildPong(), error);
    }
    if (type->string() == "pong" || type->string() == "status") {
        return true;
    }
    if (type->string() != "chat_request") {
        *error = "unsupported proxy message type: " + type->string();
        return false;
    }
    const JsonValue *id = message.Find("id");
    const JsonValue *body = message.Find("body");
    if (id == nullptr || id->type() != JsonValue::Type::String || id->string().empty() || id->string().size() > 128) {
        *error = "chat request has no valid id";
        return false;
    }
    if (body == nullptr || body->type() != JsonValue::Type::String) {
        return SendFrame(session,
            protocol::BuildError(id->string(), 400, "chat request has no body"), error);
    }

    {
        std::lock_guard<std::mutex> lock(session->state_mutex);
        if (session->closing) {
            *error = "proxy session is closing";
            return false;
        }
        ++session->pending_callbacks;
    }

    const std::string request_id = id->string();
    llm_.GenerateAsync(body->string(), [this, session, request_id](lite_llm::GenerateResponse response) {
        bool send_response = false;
        {
            std::lock_guard<std::mutex> lock(session->state_mutex);
            send_response = !session->closing;
        }

        if (send_response) {
            std::string send_error;
            const std::string payload = protocol::BuildChatResponse(request_id, response.status_code,
                "application/json; charset=utf-8", response.body);
            if (!SendFrame(session, payload, &send_error)) {
                std::cerr << "lite-server: unable to send response id=" << request_id
                          << ": " << send_error << '\n';
                {
                    std::lock_guard<std::mutex> lock(session->state_mutex);
                    session->closing = true;
                }
                (void)shutdown(session->socket_fd, SHUT_RDWR);
            } else {
                std::cerr << "lite-server: completed request id=" << request_id
                          << " status=" << response.status_code
                          << " responseBytes=" << response.body.size() << '\n';
            }
        }

        {
            std::lock_guard<std::mutex> lock(session->state_mutex);
            --session->pending_callbacks;
        }
        session->callbacks_finished.notify_all();
    });
    return true;
}

bool ReverseClient::SendFrame(int socket_fd, const std::string &payload, std::string *error) const
{
    if (payload.empty() || payload.size() > options_.max_frame_bytes ||
        payload.size() > static_cast<std::size_t>(UINT32_MAX)) {
        *error = "outgoing frame exceeds configured limit";
        return false;
    }
    const std::vector<std::uint8_t> frame = protocol::EncodeFrame(payload);
    return SendAll(socket_fd, frame.data(), frame.size(), error);
}

bool ReverseClient::SendFrame(const std::shared_ptr<SessionState> &session,
                              const std::string &payload, std::string *error) const
{
    std::lock_guard<std::mutex> lock(session->send_mutex);
    return SendFrame(session->socket_fd, payload, error);
}

bool ReverseClient::ReceiveFrame(int socket_fd, int timeout_ms, std::string *payload, std::string *error) const
{
    const auto started = std::chrono::steady_clock::now();
    auto receive_exact = [&](std::uint8_t *buffer, std::size_t size) -> bool {
        std::size_t offset = 0;
        while (offset < size && !should_stop_()) {
            if (timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                if (elapsed >= timeout_ms) {
                    *error = "receive timed out";
                    return false;
                }
            }
            pollfd descriptor{};
            descriptor.fd = socket_fd;
            descriptor.events = POLLIN;
            const int poll_result = poll(&descriptor, 1, kPollIntervalMs);
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                *error = ErrnoMessage("poll receive");
                return false;
            }
            if (poll_result == 0) {
                continue;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                *error = "proxy socket disconnected";
                return false;
            }
            const ssize_t count = recv(socket_fd, buffer + offset, size - offset, 0);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
            } else if (count == 0) {
                *error = "proxy closed the connection";
                return false;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                *error = ErrnoMessage("recv");
                return false;
            }
        }
        if (should_stop_()) {
            *error = "server is stopping";
            return false;
        }
        return offset == size;
    };

    std::array<std::uint8_t, protocol::kFrameHeaderBytes> header{};
    if (!receive_exact(header.data(), header.size())) {
        return false;
    }
    std::uint32_t payload_size = 0;
    if (!protocol::DecodeFrameHeader(header.data(), &payload_size)) {
        *error = "invalid frame magic";
        return false;
    }
    if (payload_size == 0 || payload_size > options_.max_frame_bytes) {
        *error = "incoming frame exceeds configured limit";
        return false;
    }
    std::vector<std::uint8_t> bytes(payload_size);
    if (!receive_exact(bytes.data(), bytes.size())) {
        return false;
    }
    payload->assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return true;
}

void ReverseClient::CloseSession(const std::shared_ptr<SessionState> &session) const
{
    std::unique_lock<std::mutex> lock(session->state_mutex);
    session->closing = true;
    session->callbacks_finished.wait(lock, [&session]() {
        return session->pending_callbacks == 0;
    });
}

void ReverseClient::SleepForReconnect(std::uint32_t milliseconds) const
{
    std::uint32_t remaining = milliseconds;
    while (remaining > 0 && !should_stop_()) {
        const std::uint32_t interval = std::min<std::uint32_t>(remaining, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        remaining -= interval;
    }
}

}  // namespace appless::lite_server
