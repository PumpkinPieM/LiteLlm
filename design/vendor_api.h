#ifndef LITE_LLM_DESIGN_VENDOR_API_H
#define LITE_LLM_DESIGN_VENDOR_API_H

#include <functional>
#include <string>
#include <string_view>

namespace lite_llm::design {

// A complete transport-neutral response. lite-server should forward these
// fields without interpreting model output or constructing completion JSON.
struct GenerateResponse {
    int status_code = 500;
    std::string body;
};

struct InitializeResult {
    bool ok = false;
    std::string error;
};

// Called exactly once for every GenerateAsync call, including calls rejected
// as invalid, busy, or not ready. It may run inline or on a LiteLlm-owned
// worker thread.
using CompletionCallback = std::function<void(GenerateResponse response)>;

class LiteLlm {
public:
    static LiteLlm &Instance();

    LiteLlm(const LiteLlm &) = delete;
    LiteLlm &operator=(const LiteLlm &) = delete;

    // Synchronously reads the config and creates the inference runtime. Success
    // means the model is loaded and ready to generate.
    InitializeResult Initialize(std::string_view config_path);

    // Non-blocking. Only one generation may be active. A second generation is
    // rejected through on_complete and is never queued.
    //
    // request_json uses the OpenAI chat-completions request shape. LiteLlm
    // validates it, applies the chat template, and translates generation
    // settings for the inference runtime.
    void GenerateAsync(
        std::string request_json,
        CompletionCallback on_complete);

    // Stops admission, waits for active inference to finish, joins the worker,
    // and releases the runtime. It is safe to call more than once.
    void Shutdown() noexcept;

private:
    LiteLlm() = default;
    ~LiteLlm() = default;
};

}  // namespace lite_llm::design

#endif  // LITE_LLM_DESIGN_VENDOR_API_H
