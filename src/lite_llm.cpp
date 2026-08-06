#include "lite_llm.h"

#include "inference_runtime.h"
#include "json_value.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace lite_llm {
namespace {

using internal::JsonValue;

constexpr std::size_t kMaximumMessages = 1024;
constexpr std::size_t kMaximumModelBytes = 256;
constexpr std::size_t kMaximumSessionIdBytes = 256;
constexpr std::uint32_t kMaximumNewTokens = 1U << 20U;

struct ParsedRequest {
    std::string model;
    std::string prompt;
    GenerationConfig generation;
};

struct Job {
    std::string request_json;
    CompletionCallback on_complete;
};

std::string EscapeJsonString(const std::string &value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char current : value) {
        switch (current) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (current < 0x20U) {
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

GenerateResponse ErrorResponse(int status_code, const std::string &code, const std::string &message)
{
    return {status_code,
        "{\"error\":{\"message\":" + EscapeJsonString(message) +
        ",\"type\":\"lite_llm_error\",\"code\":" + EscapeJsonString(code) + "}}"};
}

bool IsRoleValid(const std::string &role)
{
    if (role.empty() || role.size() > 32) {
        return false;
    }
    for (const unsigned char current : role) {
        if (std::isalnum(current) == 0 && current != '_' && current != '-') {
            return false;
        }
    }
    return true;
}

bool ReadMessageContent(const JsonValue &value, std::string *content, std::string *error)
{
    if (value.type() == JsonValue::Type::String) {
        *content = value.string();
        return true;
    }
    if (value.type() != JsonValue::Type::Array) {
        *error = "message content must be a string or an array of text parts";
        return false;
    }
    std::string combined;
    for (const JsonValue &part : value.array()) {
        if (part.type() != JsonValue::Type::Object) {
            continue;
        }
        const JsonValue *type = part.Find("type");
        const JsonValue *text = part.Find("text");
        if (type == nullptr || type->type() != JsonValue::Type::String || type->string() != "text" ||
            text == nullptr || text->type() != JsonValue::Type::String) {
            continue;
        }
        if (!combined.empty()) {
            combined.push_back('\n');
        }
        combined += text->string();
    }
    if (combined.empty()) {
        *error = "message content contains no supported text parts";
        return false;
    }
    *content = std::move(combined);
    return true;
}

bool ReadGenerationConfig(const JsonValue &root, GenerationConfig *config, std::string *error)
{
    const JsonValue *temperature = root.Find("temperature");
    if (temperature != nullptr) {
        if (temperature->type() != JsonValue::Type::Number || temperature->number() < 0.0 ||
            temperature->number() > 2.0) {
            *error = "temperature must be a number between 0 and 2";
            return false;
        }
        config->temperature = static_cast<float>(temperature->number());
    }

    const JsonValue *max_tokens = root.Find("max_completion_tokens");
    if (max_tokens == nullptr) {
        max_tokens = root.Find("max_tokens");
    }
    if (max_tokens != nullptr) {
        const double value = max_tokens->type() == JsonValue::Type::Number ? max_tokens->number() : -1.0;
        if (value < 1.0 || value > static_cast<double>(kMaximumNewTokens) || std::floor(value) != value) {
            *error = "max_completion_tokens must be a positive integer within the supported limit";
            return false;
        }
        config->max_new_tokens = static_cast<std::uint32_t>(value);
    }

    const JsonValue *extension = root.Find("lite_llm");
    if (extension == nullptr) {
        return true;
    }
    if (extension->type() != JsonValue::Type::Object) {
        *error = "lite_llm must be a JSON object";
        return false;
    }
    const JsonValue *session_id = extension->Find("session_id");
    if (session_id == nullptr) {
        return true;
    }
    if (session_id->type() != JsonValue::Type::String || session_id->string().empty() ||
        session_id->string().size() > kMaximumSessionIdBytes) {
        *error = "lite_llm.session_id must contain between 1 and 256 bytes";
        return false;
    }
    config->session_id = session_id->string();
    return true;
}

bool ParseRequest(const std::string &body, ParsedRequest *request, std::string *error)
{
    JsonValue root;
    if (!internal::ParseJson(body, &root, error)) {
        return false;
    }
    if (root.type() != JsonValue::Type::Object) {
        *error = "chat request must be a JSON object";
        return false;
    }

    const JsonValue *stream = root.Find("stream");
    if (stream != nullptr && (stream->type() != JsonValue::Type::Boolean || stream->boolean())) {
        std::cout << "streaming mode is not supported in LiteLlm yet." << std::endl;
    }

    const JsonValue *model = root.Find("model");
    if (model == nullptr) {
        request->model = "lite-local";
    } else if (model->type() != JsonValue::Type::String || model->string().empty() ||
        model->string().size() > kMaximumModelBytes) {
        *error = "model must contain between 1 and 256 bytes";
        return false;
    } else {
        request->model = model->string();
    }

    if (!ReadGenerationConfig(root, &request->generation, error)) {
        return false;
    }

    const JsonValue *messages = root.Find("messages");
    if (messages == nullptr || messages->type() != JsonValue::Type::Array || messages->array().empty()) {
        *error = "chat request requires a non-empty messages array";
        return false;
    }
    if (messages->array().size() > kMaximumMessages) {
        *error = "chat request contains too many messages";
        return false;
    }

    std::string prompt;
    for (const JsonValue &message : messages->array()) {
        if (message.type() != JsonValue::Type::Object) {
            *error = "each message must be a JSON object";
            return false;
        }
        const JsonValue *role = message.Find("role");
        const JsonValue *content_value = message.Find("content");
        if (role == nullptr || role->type() != JsonValue::Type::String || !IsRoleValid(role->string())) {
            *error = "each message requires a valid role";
            return false;
        }
        if (content_value == nullptr) {
            *error = "each message requires content";
            return false;
        }
        std::string content;
        if (!ReadMessageContent(*content_value, &content, error)) {
            return false;
        }
        prompt += "[" + role->string() + "]\n" + content + "\n\n";
    }
    prompt += "[assistant]\n";
    request->prompt = std::move(prompt);
    return true;
}

GenerateResponse CompletionResponse(const std::string &model, const GenerateResult &result)
{
    static std::atomic<std::uint64_t> next_id{1};
    const auto created = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string id = "chatcmpl-lite-" + std::to_string(next_id.fetch_add(1));
    const char *finish_reason = result.finish_reason == FinishReason::Length ? "length" : "stop";
    const std::uint64_t total_tokens =
        static_cast<std::uint64_t>(result.prompt_tokens) + result.generated_tokens;

    return {200,
        "{\"id\":" + EscapeJsonString(id) +
        ",\"object\":\"chat.completion\",\"created\":" + std::to_string(created) +
        ",\"model\":" + EscapeJsonString(model) +
        ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":" +
        EscapeJsonString(result.text) + "},\"finish_reason\":" + EscapeJsonString(finish_reason) +
        "}],\"usage\":{\"prompt_tokens\":" + std::to_string(result.prompt_tokens) +
        ",\"completion_tokens\":" + std::to_string(result.generated_tokens) +
        ",\"total_tokens\":" + std::to_string(total_tokens) + "}}"};
}

GenerateResponse RuntimeErrorResponse(const GenerateResult &result)
{
    const std::string message = result.error.empty() ? "inference failed" : result.error;
    switch (result.status) {
        case GenerateStatus::InvalidArgument:
            return ErrorResponse(400, "invalid_argument", message);
        case GenerateStatus::ContextTooLong:
            return ErrorResponse(400, "context_too_long", message);
        case GenerateStatus::OutOfMemory:
            return ErrorResponse(503, "out_of_memory", message);
        case GenerateStatus::InternalError:
            return ErrorResponse(500, "internal_error", message);
        case GenerateStatus::Ok:
            break;
    }
    return ErrorResponse(500, "internal_error", message);
}

}  // namespace

class LiteLlm::Impl {
public:
    InitializeResult Initialize(std::string_view config_path)
    {
        if (config_path.empty()) {
            return {false, "config path is empty"};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_ != nullptr || worker_.joinable()) {
                return {false, "LiteLlm is already initialized"};
            }
        }

        std::unique_ptr<InferenceRuntime> runtime;
        try {
            runtime = InferenceRuntime::CreateFromConfig(std::string(config_path));
        } catch (const std::exception &exception) {
            return {false, exception.what()};
        } catch (...) {
            return {false, "unknown inference runtime initialization failure"};
        }
        if (!runtime) {
            return {false, "InferenceRuntime::CreateFromConfig returned null"};
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_ = std::move(runtime);
            stopping_ = false;
            worker_ = std::thread([this]() { WorkerLoop(); });
        } catch (const std::exception &exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_.reset();
            return {false, exception.what()};
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_.reset();
            return {false, "unable to start inference worker"};
        }
        return {true, ""};
    }

    void GenerateAsync(std::string request_json, CompletionCallback on_complete)
    {
        if (!on_complete) {
            return;
        }

        GenerateResponse rejection;
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_ == nullptr || !worker_.joinable()) {
                rejection = ErrorResponse(503, "not_ready", "LiteLlm is not initialized");
                rejected = true;
            } else if (stopping_) {
                rejection = ErrorResponse(503, "shutting_down", "LiteLlm is shutting down");
                rejected = true;
            } else if (busy_) {
                rejection = ErrorResponse(409, "busy", "another generation is already active");
                rejected = true;
            } else {
                busy_ = true;
                job_ = Job{std::move(request_json), std::move(on_complete)};
                condition_.notify_one();
            }
        }
        if (rejected) {
            InvokeCallback(on_complete, std::move(rejection));
        }
    }

    void Shutdown() noexcept
    {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) {
                runtime_.reset();
                stopping_ = false;
                busy_ = false;
                job_.reset();
                return;
            }
            stopping_ = true;
            condition_.notify_one();
            worker = std::move(worker_);
        }

        try {
            worker.join();
        } catch (...) {
            if (worker.joinable()) {
                worker.detach();
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        runtime_.reset();
        job_.reset();
        stopping_ = false;
        busy_ = false;
    }

private:
    static void InvokeCallback(CompletionCallback &callback, GenerateResponse response) noexcept
    {
        try {
            callback(std::move(response));
        } catch (...) {
        }
    }

    GenerateResponse Process(const std::string &request_json) noexcept
    {
        try {
            ParsedRequest request;
            std::string error;
            if (!ParseRequest(request_json, &request, &error)) {
                return ErrorResponse(400, "invalid_request", error);
            }
            const GenerateResult result = runtime_->Generate(request.prompt, request.generation);
            return result.ok() ? CompletionResponse(request.model, result) : RuntimeErrorResponse(result);
        } catch (const std::exception &exception) {
            return ErrorResponse(500, "internal_error", exception.what());
        } catch (...) {
            return ErrorResponse(500, "internal_error", "unknown generation failure");
        }
    }

    void WorkerLoop() noexcept
    {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || job_.has_value(); });
                if (!job_.has_value()) {
                    return;
                }
                job = std::move(*job_);
                job_.reset();
            }

            GenerateResponse response = Process(job.request_json);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
            }
            InvokeCallback(job.on_complete, std::move(response));

            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ && !job_.has_value()) {
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::unique_ptr<InferenceRuntime> runtime_;
    std::optional<Job> job_;
    std::thread worker_;
    bool busy_ = false;
    bool stopping_ = false;
};

LiteLlm &LiteLlm::Instance()
{
    static LiteLlm instance;
    return instance;
}

LiteLlm::LiteLlm() : impl_(std::make_unique<Impl>()) {}

LiteLlm::~LiteLlm()
{
    Shutdown();
}

InitializeResult LiteLlm::Initialize(std::string_view config_path)
{
    return impl_->Initialize(config_path);
}

void LiteLlm::GenerateAsync(std::string request_json, CompletionCallback on_complete)
{
    impl_->GenerateAsync(std::move(request_json), std::move(on_complete));
}

void LiteLlm::Shutdown() noexcept
{
    impl_->Shutdown();
}

}  // namespace lite_llm
