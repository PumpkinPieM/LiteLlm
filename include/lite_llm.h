#ifndef LITE_LLM_LITE_LLM_H
#define LITE_LLM_LITE_LLM_H

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace lite_llm {

struct GenerateResponse {
    int status_code = 500;
    std::string body;
};

struct InitializeResult {
    bool ok = false;
    std::string error;
};

using CompletionCallback = std::function<void(GenerateResponse response)>;

class LiteLlm {
public:
    static LiteLlm &Instance();

    LiteLlm(const LiteLlm &) = delete;
    LiteLlm &operator=(const LiteLlm &) = delete;

    InitializeResult Initialize(std::string_view config_path);

    // Returns immediately. on_complete is called exactly once, including when
    // the request is rejected because LiteLlm is busy or not initialized.
    void GenerateAsync(std::string request_json, CompletionCallback on_complete);

    // Stops admission and waits for an active generation to finish.
    void Shutdown() noexcept;

private:
    class Impl;

    LiteLlm();
    ~LiteLlm();

    std::unique_ptr<Impl> impl_;
};

}  // namespace lite_llm

#endif  // LITE_LLM_LITE_LLM_H
