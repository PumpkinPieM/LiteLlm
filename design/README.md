# LiteLlm framework design draft

This directory records the design that is now implemented by:

- `../include/lite_llm.h` and `../src/lite_llm.cpp`
- `../runtime/include/inference_runtime.h`

## Boundary split

- `vendor_api.h` defines the server-facing `LiteLlm` singleton. It owns request
  parsing, chat templating, admission, asynchronous execution, and response
  encoding.
- `inference_runtime.h` is the narrow boundary implemented by the proprietary
  shared library. It loads one model from a config file and performs blocking
  generation from an already-formatted prompt.

The runtime does not parse JSON, apply chat templates, own asynchronous workers,
perform admission, or encode OpenAI responses.

## Generation request

For now the request is an ordinary OpenAI chat-completions request:

```json
{
  "model": "qwen3-0.6b",
  "messages": [
    {"role": "system", "content": "Be concise."},
    {"role": "user", "content": "Hello"}
  ],
  "temperature": 0.7,
  "max_completion_tokens": 512,
  "stream": false,
  "lite_llm": {
    "session_id": "550e8400-e29b-41d4-a716-446655440000"
  }
}
```

The complete message history is authoritative. `session_id` is stable across
turns and is passed to the runtime as a best-effort KV/prefix-cache key. The
runtime must verify the token prefix before reuse. A missing cache, eviction, or
changed history affects performance only, never correctness.

## Request flow

1. `lite-server` forwards the raw request JSON to `GenerateAsync`.
2. `LiteLlm` atomically accepts the request or rejects it as busy; requests are
   never queued. Every call completes exactly once through its callback,
   including immediate rejection.
3. `LiteLlm` validates JSON, applies the model's chat template, translates the
   session and generation config, and calls the runtime's blocking `Generate`
   on its worker.
4. The runtime tokenizes the prompt, safely reuses any matching cached prefix,
   runs inference, and returns text, finish reason, token counts, and any error.
5. `LiteLlm` encodes an OpenAI-compatible response and invokes the completion
   callback exactly once.
6. `lite-server` forwards the response using its own transport request ID.

No vendor-level request ID, explicit session deletion, streaming,
caller-defined stop sequences, request cancellation, or inference interruption
is included in this draft. `Shutdown` waits for active inference to finish.
