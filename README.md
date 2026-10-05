# Apollo LLM 1.0

Apollo LLM 1.0 is a native C chat application with an in-house Direct3D 12 inference backend and a CPU fallback. The standard profile assigns request-level model weights automatically: programming and C requests use coder 100% / language 0%; other requests use language 100% / coder 0%. Routing looks only at the latest user turn, and runs one expert per request to avoid mixing incompatible token scores. Large requests are handled as a sequence of small steps, with a larger output budget; Apollo gives the useful steps when they help and combines them into a concise result when that is clearer. Its answer confidence is a heuristic based on generated-token probabilities, task difficulty cues, and whether generation reached its token limit. Below 25%, the chat consults the original 1.5B checkpoint through a separate Apollo C inference executable. This heuristic is not a calibrated probability that an entire answer is perfect.

## Run

- `apollo_1_0_chat.exe` opens Apollo LLM 1.0 with both 0.5B models.
- Keep `apollo_teacher_infer.exe` beside the standard chat and its model folder; Apollo uses it for low-confidence reference answers.
- `apollo_compact_chat.exe` opens the coder-only compact profile.
- Keep each chat executable beside `model/` and its matching inference executable.
- `apollo_teacher_infer.exe` runs the original 1.5B checkpoint directly when called by the standard chat.

## Local conversation memory

Recent conversation context is saved in `Apollo_1.0.memory` beside the chat executable and restored when Apollo starts again. Complete teacher answers are separately stored in `Apollo_1.0.references`; a repeat with the same words (ignoring case, spacing, and punctuation) reuses its saved answer without running either model. Answers cut off at the generation limit are not cached. This is a persistent reference cache; it does not modify the models' weights. These files remain local to the computer. Delete the memory file to clear the saved conversation or the references file to clear cached teacher answers.

## Build

Run `make` with GCC and the Windows Direct3D 12 development libraries available.

## Model notices

The language checkpoint is derived from Qwen2.5-0.5B-Instruct, converted to GGUF Q5_0. Its upstream model card and Apache 2.0 license are listed in `THIRD_PARTY_NOTICES.md`; the license text is in `LICENSES/Apache-2.0.txt`. The coder checkpoint is the separately supplied compact model. Apollo's C inference engine and chat interface are local code and do not use an external inference service.
