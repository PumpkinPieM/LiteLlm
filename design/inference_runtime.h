#ifndef LITE_LLM_DESIGN_INFERENCE_RUNTIME_H
#define LITE_LLM_DESIGN_INFERENCE_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>

namespace lite_llm::design {

struct GenerationConfig {
    // Stable across turns of the same conversation. The runtime may use it for
    // best-effort KV/prefix-cache reuse; the complete prompt remains
    // authoritative.
    std::string session_id;

    std::uint32_t max_new_tokens = 512;
    float temperature = 0.7F;
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

    // Empty on success.
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == GenerateStatus::Ok;
    }
};

// Minimal boundary exported by the proprietary inference shared library.
// LiteLlm owns JSON parsing, chat templating, async execution, admission, and
// response encoding.
class InferenceRuntime {
public:
    static std::unique_ptr<InferenceRuntime> CreateFromConfig(
        const std::string &config_path);

    virtual ~InferenceRuntime() = default;

    InferenceRuntime(const InferenceRuntime &) = delete;
    InferenceRuntime &operator=(const InferenceRuntime &) = delete;

    // Synchronous and blocking. LiteLlm guarantees that Generate() is never
    // called concurrently. The prompt already includes the model's chat
    // template. The runtime tokenizes it, optionally reuses a verified prefix
    // for config.session_id, and performs inference.
    virtual GenerateResult Generate(
        const std::string &prompt,
        const GenerationConfig &config) = 0;

protected:
    InferenceRuntime() = default;
};

}  // namespace lite_llm::design

#endif  // LITE_LLM_DESIGN_INFERENCE_RUNTIME_H
