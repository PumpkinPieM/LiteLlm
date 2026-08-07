#ifndef LITE_LLM_INFERENCE_RUNTIME_H
#define LITE_LLM_INFERENCE_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>

namespace lite_llm {

struct GenerationConfig {
    // Stable across turns of one conversation. The complete prompt remains
    // authoritative; cached state must only be reused after prefix validation.
    std::string session_id;
    std::uint32_t max_new_tokens = 512;
    float temperature = 0.7F;
    bool verbose = false;
};

enum class GenerateStatus {
    Ok,
    InvalidArgument,
    ContextTooLong,
    OutOfMemory,
    InternalError,
};

enum class FinishReason {
    Stop,
    Length,
};

struct GenerateResult {
    GenerateStatus status = GenerateStatus::InternalError;
    std::string text;
    FinishReason finish_reason = FinishReason::Stop;
    std::uint32_t prompt_tokens = 0;
    std::uint32_t generated_tokens = 0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == GenerateStatus::Ok;
    }
};

class InferenceRuntime {
public:
    static std::unique_ptr<InferenceRuntime> CreateFromConfig(
        const std::string &config_path);

    virtual ~InferenceRuntime() = default;

    InferenceRuntime(const InferenceRuntime &) = delete;
    InferenceRuntime &operator=(const InferenceRuntime &) = delete;

    virtual GenerateResult Generate(
        const std::string &prompt,
        const GenerationConfig &config) = 0;

protected:
    InferenceRuntime() = default;
};

}  // namespace lite_llm

#endif  // LITE_LLM_INFERENCE_RUNTIME_H
