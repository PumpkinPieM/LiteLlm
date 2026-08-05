#ifndef APPLESS_LITE_SERVER_PROTOCOL_CODEC_H
#define APPLESS_LITE_SERVER_PROTOCOL_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace appless::lite_server::protocol {

constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::uint32_t kProtocolVersion = 1;

std::vector<std::uint8_t> EncodeFrame(const std::string &payload);
bool DecodeFrameHeader(const std::uint8_t *header, std::uint32_t *payload_size);
std::string EscapeJsonString(const std::string &value);

std::string BuildHello(const std::string &auth_token);
std::string BuildReadyStatus();
std::string BuildPong();
std::string BuildChatResponse(const std::string &request_id, int status,
                              const std::string &content_type, const std::string &body);
std::string BuildError(const std::string &request_id, int status, const std::string &message);

}  // namespace appless::lite_server::protocol

#endif
