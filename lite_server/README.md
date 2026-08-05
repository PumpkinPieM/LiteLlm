# lite-server

`lite-server` is a standalone HarmonyOS C++ executable that connects outbound
to the app-owned `@appless/lite-proxy` HAR and invokes `LiteLlm` for generation.
It never binds a socket, so it can be launched from ordinary HDC shell on the
simulator configuration where shell processes cannot bind TCP or Unix sockets.

```text
OpenAiCompatibleModel
  -> HTTP 127.0.0.1:18080
  -> in-app lite_proxy HAR
  <- persistent LTS1 connection initiated by lite-server
  -> liblite_llm.so asynchronous facade
  -> liblite_llm_runtime.so (DeepSeek-backed demo by default)
```

## LiteLlm API

The server forwards each OpenAI request body to the `LiteLlm` singleton:

```cpp
auto initialized = lite_llm::LiteLlm::Instance().Initialize(config_path);
lite_llm::LiteLlm::Instance().GenerateAsync(request_json, on_complete);
```

`LiteLlm` owns OpenAI request parsing, chat templating, single-request
admission, its inference worker, and response encoding. It calls the runtime
shared-library boundary synchronously from that worker:

```cpp
auto runtime = lite_llm::InferenceRuntime::CreateFromConfig(config_path);
runtime->Generate(prompt, generation_config);
```

The demo runtime is built as a separate `liblite_llm_runtime.so`. Its config
contains the DeepSeek chat-completions endpoint, model, and API key. Each
generation returns text, finish reason, and token usage.

## Build

From the repository root:

```powershell
.\build.ps1 -Arch x86_64 -BuildType Debug
.\build.ps1 -Arch arm64-v8a -BuildType Release
```

```bash
./build.sh --arch arm64-v8a --build-type Release
```

Outputs are written to `build-<abi>/bin` and `build-<abi>/lib`:

- `lite-server`
- `liblite_llm.so`
- `liblite_llm_runtime.so`
- `lite-server-tests`

The executable and vendor library use the same SDK `libc++_shared.so`. Deploy
that runtime beside them to keep the C++ ABI consistent.

## App configuration

Enable the proxy using a config matching the server launch values:

```json
{
  "enabled": true,
  "httpPort": 18080,
  "reversePort": 18081,
  "authToken": "replace-this-development-token",
  "requestTimeoutMs": 120000,
  "maxBodyBytes": 4194304
}
```

Place it at:

```text
entry/src/main/resources/rawfile/lite_proxy_config.json
```

The app then continues to use its existing `OpenAiCompatibleModel`, configured
with `http://127.0.0.1:18080/v1` and the same token.

## Deploy and test

For a complete connection-only simulator procedure, see
[`../../docs/lite-proxy-server-test-guide.md`](../../docs/lite-proxy-server-test-guide.md).

With the simulator or device connected:

```powershell
.\lite_server\scripts\deploy.ps1 `
  -Arch x86_64 `
  -AuthToken replace-this-development-token `
  -ReversePort 18081 `
  -RunTests
```

The script prints the foreground launch command. Start the app first, then run:

```text
hdc shell "cd /data/local/tmp/lite-server && LD_LIBRARY_PATH=. ./lite-server \
  --config ./config.json \
  --connect-host 127.0.0.1 \
  --connect-port 18081 \
  --auth-token replace-this-development-token"
```

Stop with `Ctrl+C` or `SIGTERM`. If the app is absent or restarts, the server
reconnects with bounded exponential backoff. Pending requests are not replayed.

## Request handling

The reverse protocol matches [`../../lite_proxy/README.md`](../../lite_proxy/README.md):

1. Connect outbound and send authenticated `hello`.
2. Receive `chat_request` frames containing the exact OpenAI JSON body.
3. Forward the exact body to `LiteLlm` without parsing model messages.
4. `LiteLlm` validates the request, formats the prompt, and submits one blocking
   generation to its worker. Concurrent requests are rejected as busy.
5. Return the OpenAI-compatible response from `LiteLlm` inside
   a correlated `chat_response` frame.

Streaming, cancellation, and inference interruption are not supported.

## Real runtime

See the repository [README](../README.md). Place the ABI-specific inference
runtime `.so` under `runtime/lib/<abi>/`, then build with `-RealRuntime`.
