#include "lite_llm.h"
#include "reverse_client.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void HandleSignal(int)
{
    stopRequested = 1;
}

bool ShouldStop()
{
    return stopRequested != 0;
}

struct Arguments {
    std::string config_path;
    appless::lite_server::ReverseClientOptions connection;
    bool help = false;
};

void PrintUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " --config PATH --connect-port PORT --auth-token TOKEN [options]\n"
        << "\nOptions:\n"
        << "  --config PATH                  Runtime config passed to CreateFromConfig\n"
        << "  --connect-host IPV4            Proxy address (default: 127.0.0.1)\n"
        << "  --connect-port PORT             Proxy reverse listener port\n"
        << "  --auth-token TOKEN              Shared proxy/server authentication token\n"
        << "  --max-frame-bytes BYTES         Frame limit (default: 8388608)\n"
        << "  --reconnect-initial-ms MS       Initial reconnect delay (default: 250)\n"
        << "  --reconnect-max-ms MS           Maximum reconnect delay (default: 5000)\n"
        << "  --help                          Show this help\n";
}

bool ParseUnsigned(const std::string &text, std::uint64_t maximum, std::uint64_t *value)
{
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char current : text) {
        if (current < '0' || current > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(current - '0');
        if (parsed > (maximum - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

bool ReadOption(int argc, char **argv, int *index, std::string *value, std::string *error)
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
        std::string value;
        if (option == "--help" || option == "-h") {
            arguments->help = true;
            return true;
        }
        if (!ReadOption(argc, argv, &index, &value, error)) {
            return false;
        }
        std::uint64_t parsed = 0;
        if (option == "--config") {
            arguments->config_path = value;
        } else if (option == "--connect-host") {
            arguments->connection.host = value;
        } else if (option == "--connect-port") {
            if (!ParseUnsigned(value, 65535, &parsed) || parsed == 0) {
                *error = "connect port must be an integer between 1 and 65535";
                return false;
            }
            arguments->connection.port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--auth-token") {
            arguments->connection.auth_token = value;
        } else if (option == "--max-frame-bytes") {
            if (!ParseUnsigned(value, 64U * 1024U * 1024U, &parsed) || parsed < 1024) {
                *error = "max frame bytes must be between 1024 and 67108864";
                return false;
            }
            arguments->connection.max_frame_bytes = static_cast<std::size_t>(parsed);
        } else if (option == "--reconnect-initial-ms") {
            if (!ParseUnsigned(value, 60000, &parsed) || parsed == 0) {
                *error = "initial reconnect delay must be between 1 and 60000";
                return false;
            }
            arguments->connection.reconnect_initial_ms = static_cast<std::uint32_t>(parsed);
        } else if (option == "--reconnect-max-ms") {
            if (!ParseUnsigned(value, 300000, &parsed) || parsed == 0) {
                *error = "maximum reconnect delay must be between 1 and 300000";
                return false;
            }
            arguments->connection.reconnect_max_ms = static_cast<std::uint32_t>(parsed);
        } else {
            *error = "unknown option: " + option;
            return false;
        }
    }
    if (arguments->config_path.empty()) {
        *error = "--config is required";
        return false;
    }
    if (arguments->connection.port == 0) {
        *error = "--connect-port is required";
        return false;
    }
    if (arguments->connection.auth_token.empty() || arguments->connection.auth_token.size() > 512) {
        *error = "--auth-token must contain between 1 and 512 bytes";
        return false;
    }
    if (arguments->connection.reconnect_initial_ms > arguments->connection.reconnect_max_ms) {
        *error = "initial reconnect delay must not exceed maximum reconnect delay";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    Arguments arguments;
    std::string error;
    if (!ParseArguments(argc, argv, &arguments, &error)) {
        std::cerr << "lite-server: " << error << "\n\n";
        PrintUsage(argv[0]);
        return 2;
    }
    if (arguments.help) {
        PrintUsage(argv[0]);
        return 0;
    }

    lite_llm::LiteLlm &llm = lite_llm::LiteLlm::Instance();
    const lite_llm::InitializeResult initialized = llm.Initialize(arguments.config_path);
    if (!initialized.ok) {
        std::cerr << "lite-server: unable to initialize LiteLlm: " << initialized.error << '\n';
        return 3;
    }
    std::cerr << "lite-server: LiteLlm initialized from " << arguments.config_path << '\n';

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    appless::lite_server::ReverseClient client(arguments.connection, llm, ShouldStop);
    const int result = client.Run();
    llm.Shutdown();
    return result;
}
