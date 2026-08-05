#include "json_value.h"
#include "lite_llm.h"
#include "protocol_codec.h"

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace lite = appless::lite_server;
namespace protocol = appless::lite_server::protocol;

namespace {

int failures = 0;

void Expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestJson()
{
    lite::JsonValue value;
    std::string error;
    Expect(lite::ParseJson("{\"text\":\"line\\n\\u4f60\\u597d\",\"number\":1.5}", &value, &error),
        "parse JSON object");
    const lite::JsonValue *text = value.Find("text");
    Expect(text != nullptr && text->type() == lite::JsonValue::Type::String && text->string() == "line\n你好",
        "decode JSON string");
    Expect(!lite::ParseJson("{\"text\":\"\\udc00\"}", &value, &error), "reject unpaired surrogate");
    Expect(!lite::ParseJson("[1,]", &value, &error), "reject trailing comma");
}

void TestProtocol()
{
    const std::string payload = protocol::BuildHello("secret-token");
    const std::vector<std::uint8_t> frame = protocol::EncodeFrame(payload);
    std::uint32_t size = 0;
    Expect(frame.size() == payload.size() + protocol::kFrameHeaderBytes, "frame size");
    Expect(protocol::DecodeFrameHeader(frame.data(), &size) && size == payload.size(), "frame header");

    lite::JsonValue hello;
    std::string error;
    Expect(lite::ParseJson(payload, &hello, &error), "hello is valid JSON");
    const lite::JsonValue *token = hello.Find("auth_token");
    Expect(token != nullptr && token->type() == lite::JsonValue::Type::String && token->string() == "secret-token",
        "hello token");
}

void TestLiteLlmNotReady()
{
    bool called = false;
    lite_llm::GenerateResponse response;
    lite_llm::LiteLlm::Instance().GenerateAsync("{}",
        [&called, &response](lite_llm::GenerateResponse value) {
            called = true;
            response = std::move(value);
        });
    Expect(called, "not-ready callback is invoked inline");
    Expect(response.status_code == 503, "not-ready status");

    lite::JsonValue body;
    std::string error;
    Expect(lite::ParseJson(response.body, &body, &error), "not-ready body is valid JSON");
    Expect(body.Find("error") != nullptr, "not-ready body contains an error");
}

void TestDeepSeekVendor(const std::string &config_path)
{
    if (config_path.empty()) {
        return;
    }
    lite_llm::LiteLlm &llm = lite_llm::LiteLlm::Instance();
    const lite_llm::InitializeResult initialized = llm.Initialize(config_path);
    Expect(initialized.ok, "initialize DeepSeek runtime: " + initialized.error);
    if (!initialized.ok) {
        return;
    }

    std::mutex mutex;
    std::condition_variable finished;
    bool called = false;
    lite_llm::GenerateResponse response;
    llm.GenerateAsync(
        "{\"model\":\"mock-model\",\"messages\":[{\"role\":\"user\","
        "\"content\":\"Reply with exactly: lite-server-ok\"}],\"temperature\":0.2,"
        "\"max_completion_tokens\":32,\"stream\":false,\"lite_llm\":{"
        "\"session_id\":\"test-session\"}}",
        [&mutex, &finished, &called, &response](lite_llm::GenerateResponse value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                response = std::move(value);
                called = true;
            }
            finished.notify_one();
        });
    {
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [&called]() { return called; });
    }
    Expect(response.status_code == 200, "generate through DeepSeek runtime: " + response.body);

    lite::JsonValue completion;
    std::string error;
    Expect(lite::ParseJson(response.body, &completion, &error), "completion is valid JSON");
    const lite::JsonValue *choices = completion.Find("choices");
    Expect(choices != nullptr && choices->type() == lite::JsonValue::Type::Array &&
        !choices->array().empty(), "completion contains choices");
    Expect(completion.Find("usage") != nullptr, "completion contains token usage");
    llm.Shutdown();
}

}  // namespace

int main(int argc, char **argv)
{
    TestJson();
    TestProtocol();
    TestLiteLlmNotReady();
    TestDeepSeekVendor(argc > 1 ? argv[1] : "");
    if (failures != 0) {
        return 1;
    }
    std::cout << "lite-server-tests: PASS\n";
    return 0;
}
