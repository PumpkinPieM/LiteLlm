# LiteLlm framework and runtime boundary

The server uses the open `LiteLlm` facade in `include/lite_llm.h`:

```cpp
auto initialized = lite_llm::LiteLlm::Instance().Initialize(config_path);
lite_llm::LiteLlm::Instance().GenerateAsync(request_json, on_complete);
```

The facade parses OpenAI requests, formats prompts, owns asynchronous execution,
and constructs OpenAI responses. The proprietary inference boundary remains
limited to `CreateFromConfig` and blocking `Generate` in
`runtime/include/inference_runtime.h`.

The bundled implementation under `runtime/mock/` is selected by default with
`-DLITE_LLM_USE_MOCK_RUNTIME=ON`. It builds as `liblite_llm_runtime.so`, preserving
the runtime shared-library boundary while sending prompts to DeepSeek over
HTTPS.

For the real runtime, configure with `-DLITE_LLM_USE_MOCK_RUNTIME=OFF` and provide:

```text
lite_llm/
  include/lite_llm.h
  runtime/
    include/inference_runtime.h
    lib/
      arm64-v8a/liblite_llm_runtime.so
      x86_64/liblite_llm_runtime.so
```

All libraries crossing this C++ ABI must use a compatible HarmonyOS NDK,
compiler generation, architecture, and `libc++_shared.so`.

## Build

Build the shared library and standalone server together on the host:

```bash
./build.sh
```

Build for HarmonyOS:

```bash
./build.sh --arch arm64-v8a --build-type Release
```

```powershell
.\build.ps1 -Arch arm64-v8a -BuildType Release
```

Outputs are placed under `build-<arch>/`:

```text
bin/lite-server
bin/lite-server-tests
lib/liblite_llm.so
lib/liblite_llm_runtime.so
```

`lite_server/` also has its own `CMakeLists.txt`. It can be configured as a
standalone project by setting `LITE_LLM_INCLUDE_DIR` and `LITE_LLM_LIBRARY` to
an already-built LiteLlm installation.

## DeepSeek demo configuration

The demo reads its endpoint, model, and API key from the config:

```json
{
  "endpoint": "https://api.deepseek.com/chat/completions",
  "model": "deepseek-v4-flash",
  "apiKey": "..."
}
```

`Generate` sends the formatted prompt and generation settings to DeepSeek,
prints the returned assistant content, and maps its finish reason and token
usage into `GenerateResult`. The demo ignores `session_id` because the remote
API does not expose its physical KV cache. HTTPS is provided by the pinned Mbed
TLS dependency configured in the top-level CMake project. Because the demo
runtime has no portable CA-bundle path, this local-demo transport encrypts the
connection but does not verify the server certificate.
