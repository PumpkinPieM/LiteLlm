#include "inference_runtime.h"

#if defined(__OHOS__)
#include <hilog/log.h>
#endif
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr const char *MOCK_LOG_TAG = "LiteLlmDeepSeek";
#if defined(__OHOS__)
constexpr unsigned int MOCK_LOG_DOMAIN = 0x0000;
#define LITE_LLM_LOG_COMPLETION(size) \
    OH_LOG_Print(LOG_APP, LOG_INFO, MOCK_LOG_DOMAIN, MOCK_LOG_TAG, \
        "DeepSeek Generate completed responseBytes=%{public}zu", size)
#else
#define LITE_LLM_LOG_COMPLETION(size) ((void)(size))
#endif
constexpr const char *DEFAULT_ENDPOINT = "https://api.deepseek.com/chat/completions";
constexpr const char *DEFAULT_MODEL = "deepseek-v4-flash";
constexpr unsigned int READ_TIMEOUT_MS = 120000;

struct Url {
    std::string host;
    std::string port;
    std::string path;
};

class TlsConnection {
public:
    TlsConnection()
    {
        mbedtls_net_init(&socket_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_entropy_init(&entropy_);
    }

    ~TlsConnection()
    {
        mbedtls_ssl_close_notify(&ssl_);
        mbedtls_net_free(&socket_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
    }

    std::string Post(const Url &url, const std::string &apiKey, const std::string &body)
    {
        Check(mbedtls_ctr_drbg_seed(&random_, mbedtls_entropy_func, &entropy_,
            reinterpret_cast<const unsigned char *>(MOCK_LOG_TAG), std::char_traits<char>::length(MOCK_LOG_TAG)),
            "seed TLS random generator");
        Check(mbedtls_ssl_config_defaults(
            &config_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT),
            "configure TLS");

        // The demo runtime has no portable CA-bundle path. Traffic remains encrypted,
        // but peer verification is intentionally disabled for this local demo.
        mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &random_);
        mbedtls_ssl_conf_read_timeout(&config_, READ_TIMEOUT_MS);
        Check(mbedtls_ssl_setup(&ssl_, &config_), "initialize TLS session");
        Check(mbedtls_ssl_set_hostname(&ssl_, url.host.c_str()), "set TLS hostname");
        Check(mbedtls_net_connect(&socket_, url.host.c_str(), url.port.c_str(), MBEDTLS_NET_PROTO_TCP),
            "connect to DeepSeek");
        mbedtls_ssl_set_bio(&ssl_, &socket_, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

        int result = 0;
        do {
            result = mbedtls_ssl_handshake(&ssl_);
        } while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
        Check(result, "complete TLS handshake");

        const std::string request =
            "POST " + url.path + " HTTP/1.1\r\n"
            "Host: " + url.host + "\r\n"
            "Authorization: Bearer " + apiKey + "\r\n"
            "Content-Type: application/json\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        WriteAll(request);
        return ReadAll();
    }

private:
    static void Check(int result, const char *action)
    {
        if (result >= 0) {
            return;
        }
        char detail[160] = {};
        mbedtls_strerror(result, detail, sizeof(detail));
        throw std::runtime_error(std::string("Unable to ") + action + ": " + detail);
    }

    void WriteAll(const std::string &data)
    {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const int result = mbedtls_ssl_write(&ssl_,
                reinterpret_cast<const unsigned char *>(data.data() + offset), data.size() - offset);
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            Check(result, "send DeepSeek request");
            offset += static_cast<std::size_t>(result);
        }
    }

    std::string ReadAll()
    {
        std::string response;
        unsigned char buffer[4096];
        for (;;) {
            const int result = mbedtls_ssl_read(&ssl_, buffer, sizeof(buffer));
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
                result == MBEDTLS_ERR_SSL_CONN_EOF || result == MBEDTLS_ERR_NET_CONN_RESET) {
                return response;
            }
            Check(result, "read DeepSeek response");
            response.append(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(result));
        }
    }

    mbedtls_net_context socket_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config config_;
    mbedtls_ctr_drbg_context random_;
    mbedtls_entropy_context entropy_;
};

std::string ReadFile(const std::string &path)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open DeepSeek config: " + path);
    }
    std::ostringstream content;
    content << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error("Unable to read DeepSeek config: " + path);
    }
    return content.str();
}

std::string JsonStringValue(const std::string &json, const std::string &key, const std::string &fallback = {})
{
    const std::string token = "\"" + key + "\"";
    const std::size_t keyStart = json.find(token);
    if (keyStart == std::string::npos) {
        if (!fallback.empty()) {
            return fallback;
        }
        throw std::runtime_error("DeepSeek config is missing string entry: " + key);
    }
    std::size_t cursor = json.find(':', keyStart + token.size());
    if (cursor == std::string::npos) {
        throw std::runtime_error("DeepSeek config entry has no value: " + key);
    }
    ++cursor;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
        ++cursor;
    }
    if (cursor >= json.size() || json[cursor] != '"') {
        throw std::runtime_error("DeepSeek config entry must be a string: " + key);
    }
    ++cursor;

    std::string value;
    bool escaped = false;
    for (; cursor < json.size(); ++cursor) {
        const char current = json[cursor];
        if (escaped) {
            switch (current) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(current); break;
            }
            escaped = false;
        } else if (current == '\\') {
            escaped = true;
        } else if (current == '"') {
            return value;
        } else {
            value.push_back(current);
        }
    }
    throw std::runtime_error("DeepSeek config entry is not terminated: " + key);
}

std::string EscapeJson(const std::string &value)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(HEX[byte >> 4]);
                    escaped.push_back(HEX[byte & 0x0f]);
                } else {
                    escaped.push_back(static_cast<char>(byte));
                }
        }
    }
    return escaped;
}

Url ParseHttpsUrl(const std::string &endpoint)
{
    constexpr const char *PREFIX = "https://";
    if (endpoint.compare(0, std::char_traits<char>::length(PREFIX), PREFIX) != 0) {
        throw std::runtime_error("DeepSeek endpoint must use https://");
    }
    const std::size_t authorityStart = std::char_traits<char>::length(PREFIX);
    const std::size_t pathStart = endpoint.find('/', authorityStart);
    const std::string authority = endpoint.substr(authorityStart, pathStart - authorityStart);
    if (authority.empty()) {
        throw std::runtime_error("DeepSeek endpoint has no host");
    }
    const std::size_t portStart = authority.rfind(':');
    Url url;
    if (portStart == std::string::npos) {
        url.host = authority;
        url.port = "443";
    } else {
        url.host = authority.substr(0, portStart);
        url.port = authority.substr(portStart + 1);
    }
    url.path = pathStart == std::string::npos ? "/" : endpoint.substr(pathStart);
    return url;
}

std::string DecodeChunkedBody(const std::string &body)
{
    std::string decoded;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t lineEnd = body.find("\r\n", cursor);
        if (lineEnd == std::string::npos) {
            throw std::runtime_error("DeepSeek returned malformed chunked HTTP");
        }
        const std::string sizeText = body.substr(cursor, lineEnd - cursor);
        char *end = nullptr;
        const unsigned long size = std::strtoul(sizeText.c_str(), &end, 16);
        if (end == sizeText.c_str() || (*end != '\0' && *end != ';')) {
            throw std::runtime_error("DeepSeek returned an invalid HTTP chunk size");
        }
        cursor = lineEnd + 2;
        if (size == 0) {
            return decoded;
        }
        if (size > body.size() - cursor || body.size() - cursor - size < 2) {
            throw std::runtime_error("DeepSeek returned a truncated HTTP chunk");
        }
        decoded.append(body, cursor, size);
        cursor += size;
        if (body.compare(cursor, 2, "\r\n") != 0) {
            throw std::runtime_error("DeepSeek returned a malformed HTTP chunk terminator");
        }
        cursor += 2;
    }
}

std::string HttpBody(const std::string &response)
{
    const std::size_t statusEnd = response.find("\r\n");
    const std::size_t headersEnd = response.find("\r\n\r\n");
    if (statusEnd == std::string::npos || headersEnd == std::string::npos) {
        throw std::runtime_error("DeepSeek returned a malformed HTTP response");
    }
    const std::string statusLine = response.substr(0, statusEnd);
    const std::size_t firstSpace = statusLine.find(' ');
    const int status = firstSpace == std::string::npos ? 0 : std::atoi(statusLine.c_str() + firstSpace + 1);
    const std::string headers = response.substr(statusEnd + 2, headersEnd - statusEnd - 2);
    std::string lowerHeaders = headers;
    for (char &byte : lowerHeaders) {
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    }
    std::string body = response.substr(headersEnd + 4);
    if (lowerHeaders.find("transfer-encoding: chunked") != std::string::npos) {
        body = DecodeChunkedBody(body);
    }
    if (status < 200 || status >= 300) {
        throw std::runtime_error("DeepSeek HTTP " + std::to_string(status) + ": " + body);
    }
    return body;
}

unsigned int HexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned int>(value - 'A' + 10);
    }
    throw std::runtime_error("DeepSeek response contains an invalid Unicode escape");
}

unsigned int DecodeHex4(const std::string &json, std::size_t cursor)
{
    if (json.size() - cursor < 4) {
        throw std::runtime_error("DeepSeek response contains a truncated Unicode escape");
    }
    unsigned int value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 4) | HexDigit(json[cursor + index]);
    }
    return value;
}

void AppendUtf8(std::string &output, unsigned int codePoint)
{
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

std::string DecodeJsonStringAt(const std::string &json, std::size_t cursor)
{
    if (cursor >= json.size() || json[cursor] != '"') {
        throw std::runtime_error("DeepSeek response content is not a JSON string");
    }
    ++cursor;
    std::string value;
    while (cursor < json.size()) {
        const char current = json[cursor++];
        if (current == '"') {
            return value;
        }
        if (current != '\\') {
            value.push_back(current);
            continue;
        }
        if (cursor >= json.size()) {
            break;
        }
        const char escape = json[cursor++];
        switch (escape) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                unsigned int codePoint = DecodeHex4(json, cursor);
                cursor += 4;
                if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                    if (json.size() - cursor < 6 || json[cursor] != '\\' || json[cursor + 1] != 'u') {
                        throw std::runtime_error("DeepSeek response contains an unpaired Unicode surrogate");
                    }
                    const unsigned int low = DecodeHex4(json, cursor + 2);
                    if (low < 0xdc00 || low > 0xdfff) {
                        throw std::runtime_error("DeepSeek response contains an invalid Unicode surrogate pair");
                    }
                    cursor += 6;
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                    throw std::runtime_error("DeepSeek response contains an unpaired Unicode surrogate");
                }
                AppendUtf8(value, codePoint);
                break;
            }
            default:
                throw std::runtime_error("DeepSeek response contains an unsupported JSON escape");
        }
    }
    throw std::runtime_error("DeepSeek response contains an unterminated JSON string");
}

std::string CompletionContent(const std::string &json)
{
    const std::size_t choices = json.find("\"choices\"");
    const std::size_t contentKey = json.find("\"content\"", choices);
    if (choices == std::string::npos || contentKey == std::string::npos) {
        throw std::runtime_error("DeepSeek response has no choices[0].message.content: " + json);
    }
    const std::size_t colon = json.find(':', contentKey + std::char_traits<char>::length("\"content\""));
    if (colon == std::string::npos) {
        throw std::runtime_error("DeepSeek response has malformed message content");
    }
    std::size_t valueStart = colon + 1;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart]))) {
        ++valueStart;
    }
    return DecodeJsonStringAt(json, valueStart);
}

std::uint32_t JsonUnsignedValue(const std::string &json, const std::string &key)
{
    const std::string token = "\"" + key + "\"";
    const std::size_t keyStart = json.find(token);
    if (keyStart == std::string::npos) {
        return 0;
    }
    const std::size_t colon = json.find(':', keyStart + token.size());
    if (colon == std::string::npos) {
        return 0;
    }
    std::size_t cursor = colon + 1;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
        ++cursor;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(json.c_str() + cursor, &end, 10);
    if (end == json.c_str() + cursor || value > UINT32_MAX) {
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}
} // namespace

namespace lite_llm {
namespace {

class DeepSeekRuntime final : public InferenceRuntime {
public:
    DeepSeekRuntime(std::string apiKey, std::string model, std::string endpoint)
        : apiKey_(std::move(apiKey)), model_(std::move(model)), endpoint_(std::move(endpoint))
    {
    }

    GenerateResult Generate(const std::string &prompt, const GenerationConfig &config) override
    {
        try {
            const Url url = ParseHttpsUrl(endpoint_);
            const std::string requestBody =
                "{\"model\":\"" + EscapeJson(model_) +
                "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + EscapeJson(prompt) +
                "\"}],\"temperature\":" + std::to_string(config.temperature) +
                ",\"max_tokens\":" + std::to_string(config.max_new_tokens) +
                ",\"stream\":false}";
            TlsConnection connection;
            const std::string responseBody = HttpBody(connection.Post(url, apiKey_, requestBody));
            const std::string content = CompletionContent(responseBody);
            const std::string finishReason = JsonStringValue(responseBody, "finish_reason", "stop");

            // Match the production runtime's stdout streaming contract. The
            // caller owns line termination after Generate returns.
            std::cout << content << std::flush;
            LITE_LLM_LOG_COMPLETION(content.size());

            GenerateResult result;
            result.status = GenerateStatus::Ok;
            result.text = content;
            result.finish_reason = finishReason == "length" ? FinishReason::Length : FinishReason::Stop;
            result.prompt_tokens = JsonUnsignedValue(responseBody, "prompt_tokens");
            result.generated_tokens = JsonUnsignedValue(responseBody, "completion_tokens");
            return result;
        } catch (const std::exception &exception) {
            GenerateResult result;
            result.status = GenerateStatus::InternalError;
            result.error = exception.what();
            return result;
        } catch (...) {
            GenerateResult result;
            result.status = GenerateStatus::InternalError;
            result.error = "unknown DeepSeek runtime failure";
            return result;
        }
    }

private:
    std::string apiKey_;
    std::string model_;
    std::string endpoint_;
};

}  // namespace

std::unique_ptr<InferenceRuntime> InferenceRuntime::CreateFromConfig(const std::string &configPath)
{
    const std::string config = ReadFile(configPath);
    return std::make_unique<DeepSeekRuntime>(
        JsonStringValue(config, "apiKey"),
        JsonStringValue(config, "model", DEFAULT_MODEL),
        JsonStringValue(config, "endpoint", DEFAULT_ENDPOINT));
}
} // namespace lite_llm
