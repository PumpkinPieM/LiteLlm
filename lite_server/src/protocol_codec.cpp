#include "protocol_codec.h"

#include <sstream>

namespace appless::lite_server::protocol {
namespace {

constexpr std::uint8_t kMagic[4] = {'L', 'T', 'S', '1'};

}  // namespace

std::vector<std::uint8_t> EncodeFrame(const std::string &payload)
{
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame(kFrameHeaderBytes + payload.size());
    for (std::size_t index = 0; index < 4; ++index) {
        frame[index] = kMagic[index];
    }
    frame[4] = static_cast<std::uint8_t>((size >> 24U) & 0xFFU);
    frame[5] = static_cast<std::uint8_t>((size >> 16U) & 0xFFU);
    frame[6] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
    frame[7] = static_cast<std::uint8_t>(size & 0xFFU);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        frame[kFrameHeaderBytes + index] = static_cast<std::uint8_t>(payload[index]);
    }
    return frame;
}

bool DecodeFrameHeader(const std::uint8_t *header, std::uint32_t *payload_size)
{
    if (header == nullptr || payload_size == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (header[index] != kMagic[index]) {
            return false;
        }
    }
    *payload_size = (static_cast<std::uint32_t>(header[4]) << 24U) |
        (static_cast<std::uint32_t>(header[5]) << 16U) |
        (static_cast<std::uint32_t>(header[6]) << 8U) |
        static_cast<std::uint32_t>(header[7]);
    return true;
}

std::string EscapeJsonString(const std::string &value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char current : value) {
        switch (current) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (current < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    output << "\\u00" << kHex[current >> 4U] << kHex[current & 0x0FU];
                } else {
                    output << static_cast<char>(current);
                }
        }
    }
    output << '"';
    return output.str();
}

std::string BuildHello(const std::string &auth_token)
{
    return "{\"type\":\"hello\",\"protocol\":1,\"auth_token\":" +
        EscapeJsonString(auth_token) + "}";
}

std::string BuildReadyStatus()
{
    return "{\"type\":\"status\",\"state\":\"ready\"}";
}

std::string BuildPong()
{
    return "{\"type\":\"pong\"}";
}

std::string BuildChatResponse(const std::string &request_id, int status,
                              const std::string &content_type, const std::string &body)
{
    return "{\"type\":\"chat_response\",\"id\":" + EscapeJsonString(request_id) +
        ",\"status\":" + std::to_string(status) + ",\"content_type\":" +
        EscapeJsonString(content_type) + ",\"body\":" + EscapeJsonString(body) + "}";
}

std::string BuildError(const std::string &request_id, int status, const std::string &message)
{
    return "{\"type\":\"error\",\"id\":" + EscapeJsonString(request_id) +
        ",\"status\":" + std::to_string(status) + ",\"message\":" +
        EscapeJsonString(message) + "}";
}

}  // namespace appless::lite_server::protocol
