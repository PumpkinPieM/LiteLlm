#include "inference_runtime.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Arguments {
    std::string config_path;
    std::string prompt_path;
    bool interactive = false;
    bool help = false;
};

struct Message {
    std::string role;
    std::string content;
};

void PrintUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " -j CONFIG (-p PROMPT | -i)\n"
        << "\nOptions:\n"
        << "  -j PATH  Inference runtime configuration file\n"
        << "  -p PATH  Read a prompt from a file and generate one response\n"
        << "  -i       Start an interactive, multi-turn chat\n"
        << "  -h       Show this help\n";
}

bool ReadOptionValue(int argc, char **argv, int *index, std::string *value,
    std::string *error)
{
    if (*index + 1 >= argc) {
        *error = std::string("missing value for ") + argv[*index];
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool ParseArguments(int argc, char **argv, Arguments *arguments, std::string *error)
{
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "-h" || option == "--help") {
            arguments->help = true;
            return true;
        }
        if (option == "-i") {
            arguments->interactive = true;
        } else if (option == "-j") {
            if (!ReadOptionValue(argc, argv, &index, &arguments->config_path, error)) {
                return false;
            }
        } else if (option == "-p") {
            if (!ReadOptionValue(argc, argv, &index, &arguments->prompt_path, error)) {
                return false;
            }
        } else {
            *error = "unknown option: " + option;
            return false;
        }
    }

    if (arguments->config_path.empty()) {
        *error = "-j is required";
        return false;
    }
    if (arguments->interactive == !arguments->prompt_path.empty()) {
        *error = "choose exactly one input mode: -p or -i";
        return false;
    }
    return true;
}

std::string BuildQwen3Prompt(const std::vector<Message> &messages)
{
    std::string prompt;
    for (const Message &message : messages) {
        prompt += "<|im_start|>" + message.role + "\n" + message.content +
            "<|im_end|>\n";
    }
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

lite_llm::GenerationConfig ChatGenerationConfig()
{
    lite_llm::GenerationConfig config;
    // The prompt already contains the Qwen3 chat template, so the runtime
    // must pass it to the tokenizer without applying its own template.
    config.verbose = true;
    return config;
}

bool Generate(lite_llm::InferenceRuntime &runtime, const std::string &prompt,
    lite_llm::GenerationConfig config, std::string *text, std::string *error)
{
    lite_llm::GenerateResult result;
    try {
        result = runtime.Generate(prompt, config);
    } catch (const std::exception &exception) {
        *error = exception.what();
        return false;
    } catch (...) {
        *error = "unknown inference runtime failure";
        return false;
    }

    if (!result.ok()) {
        *error = result.error.empty() ? "inference failed" : result.error;
        return false;
    }
    *text = std::move(result.text);
    return true;
}

void FinishStreamedResponse(const std::string &response)
{
    // Generate streams tokens to stdout. Terminate the line before the next
    // interactive prompt without printing the completed response a second
    // time.
    if (response.empty() || response.back() != '\n') {
        std::cout << '\n';
    }
    std::cout << std::flush;
}

bool ReadFile(const std::string &path, std::string *contents, std::string *error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "unable to open prompt file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        *error = "unable to read prompt file: " + path;
        return false;
    }
    *contents = buffer.str();
    if (contents->empty()) {
        *error = "prompt file is empty: " + path;
        return false;
    }
    return true;
}

int RunFilePrompt(lite_llm::InferenceRuntime &runtime, const std::string &path)
{
    std::string prompt;
    std::string error;
    if (!ReadFile(path, &prompt, &error)) {
        std::cerr << "lite-llm-chat: " << error << '\n';
        return 4;
    }

    std::string response;
    const std::vector<Message> messages = {{"user", std::move(prompt)}};
    if (!Generate(runtime, BuildQwen3Prompt(messages), ChatGenerationConfig(),
        &response, &error)) {
        std::cerr << "lite-llm-chat: generation failed: " << error << '\n';
        return 5;
    }
    FinishStreamedResponse(response);
    return 0;
}

int RunInteractive(lite_llm::InferenceRuntime &runtime)
{
    std::cerr
        << "Interactive mode. Paste any number of lines, then type /send on its own line.\n"
        << "Type /clear to start a new chat or /quit to exit. Blank lines are preserved.\n";

    std::vector<Message> history;
    lite_llm::GenerationConfig config = ChatGenerationConfig();
    std::uint64_t conversation_id = 1;
    config.session_id = "lite-llm-chat-" + std::to_string(conversation_id);
    for (;;) {
        std::cerr << "you> " << std::flush;
        std::string prompt;
        std::string line;
        bool has_prompt_line = false;
        bool clear_requested = false;
        while (std::getline(std::cin, line)) {
            if (line == "/quit") {
                return 0;
            }
            if (line == "/clear") {
                clear_requested = true;
                break;
            }
            if (line == "/send") {
                break;
            }
            if (has_prompt_line) {
                prompt.push_back('\n');
            }
            prompt += line;
            has_prompt_line = true;
        }

        const bool input_closed = !std::cin;
        if (clear_requested) {
            history.clear();
            config.session_id = "lite-llm-chat-" + std::to_string(++conversation_id);
            std::cerr << "Conversation cleared.\n";
            continue;
        }
        if (!has_prompt_line) {
            if (input_closed) {
                return 0;
            }
            std::cerr << "Prompt is empty; enter text before /send.\n";
            continue;
        }

        history.push_back({"user", std::move(prompt)});
        std::string response;
        std::string error;
        if (!Generate(runtime, BuildQwen3Prompt(history), config, &response, &error)) {
            history.pop_back();
            std::cerr << "lite-llm-chat: generation failed: " << error << '\n';
            if (input_closed) {
                return 5;
            }
            continue;
        }
        FinishStreamedResponse(response);
        history.push_back({"assistant", std::move(response)});
        if (input_closed) {
            return 0;
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    // Keep runtime token writes visible immediately when it streams through
    // the process-wide C++ stdout stream.
    std::cout << std::unitbuf;

    Arguments arguments;
    std::string error;
    if (!ParseArguments(argc, argv, &arguments, &error)) {
        std::cerr << "lite-llm-chat: " << error << "\n\n";
        PrintUsage(argv[0]);
        return 2;
    }
    if (arguments.help) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::unique_ptr<lite_llm::InferenceRuntime> runtime;
    try {
        runtime = lite_llm::InferenceRuntime::CreateFromConfig(arguments.config_path);
    } catch (const std::exception &exception) {
        error = exception.what();
    } catch (...) {
        error = "unknown inference runtime initialization failure";
    }
    if (!runtime) {
        if (error.empty()) {
            error = "InferenceRuntime::CreateFromConfig returned null";
        }
        std::cerr << "lite-llm-chat: unable to initialize inference runtime: "
                  << error << '\n';
        return 3;
    }

    return arguments.interactive
        ? RunInteractive(*runtime) : RunFilePrompt(*runtime, arguments.prompt_path);
}
