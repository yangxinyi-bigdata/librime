# AIPARA C++ Plugin Migration Plan

This document outlines the staged work required to migrate the Lua-based AIPARA schema extensions into native C++ plugins under `plugins/aipara/`. Each task is sized for an AI-assisted implementation workflow, with clear checkpoints and integration work.

## Phase 0 – Discovery & Project Wiring
- Audit the existing Lua modules to capture behaviours, entry points, and shared state; produce concise specs for each plugin (inputs, outputs, side effects).
- Inventory current C++ plugin hooks, schema references, and build targets; ensure `plugins/aipara` is wired into the top-level CMake so new components load in development builds.
- Define coding guidelines (naming, logging, threading, ownership) to keep the port consistent and safe.

## Phase 1 – Shared Infrastructure
- **Logger:** Design a `Logger` helper in `plugins/aipara/src/common/` that mirrors the Lua logger capabilities (levels, rotating file, optional console echo). Integrate with Rime’s `Log` facilities and expose lightweight macros for module use.
- **Text Formatting:** Port `text_formatting.lua` to a C++ utility namespace (string trimming, splitting, template helpers). Add unit tests to lock behavioural parity, referencing Lua expectations.
- **Socket Bridge:** Evaluate whether the synchronous TCP helper remains necessary. If yes, wrap it in a RAII C++ class using POSIX sockets (or existing abstractions) with timeout support and minimal dependencies. Document threading expectations clearly.
- Provide a shared configuration accessor that encapsulates repeated schema lookups (`ai_assistant/...` etc.) and caches results with invalidation hooks.

## Phase 2 – Core Processors (Input Stage)
1. **smart_cursor_processor**
   - Reconstruct the Lua logic in a `Processor` subclass; pay attention to cursor movement, selection handling, and schema flags.
   - Add defensive guards for unexpected key sequences; include verbose logging behind a debug flag.
2. **cloud_input_processor**
   - Implement lifecycle management around shared config/state updates (per the Lua orchestrator role).
   - Ensure thread-safe coordination with translators/filters; add hooks to refresh configuration when the schema changes.
3. Create integration tests (or scripted scenarios) that simulate key events to confirm processors chain correctly.

## Phase 3 – Segmentors & Translators
1. **ai_assistant_segmentor**
   - Mirror the segmentation pipeline, including trigger parsing, reply routing, and property storage.
   - Reuse shared utilities for config caching and logging; confirm behaviour with long inputs and edge-case triggers.
2. **rawenglish_segment**
   - Implement direct English segmentation with minimal dependencies; treat ASCII handling carefully and validate with mixed input cases.
3. **ai_assistant_translator**
   - Port message-generation and interaction with cloud services or cached responses. If networking is required, rely on the shared socket helper or plan for asynchronous extensions.
4. **rawenglish_translator**
   - Handle pass-through English translations, fallback candidates, and punctuation compatibility.
5. Build targeted regression tests (fixture-based) to compare Lua vs C++ candidate lists for representative inputs.

## Phase 4 – Filters & Post-Processing
1. **aux_code_filter_v3**
   - Translate filtering logic, making sure candidate mutation preserves unicode safety and tags.
2. **cloud_ai_filter_v2**
   - Implement AI response injection, respecting rate limits and schema options; reuse networking utils.
3. **punct_eng_chinese_filter**
   - Port punctuation conversion and bilingual toggles; document locale-specific edge cases.
4. For each filter, add smoke tests that feed mock candidate streams and assert on outputs, leveraging Rime’s test harness or bespoke mocks.

## Phase 5 – Logging & Diagnostics
- Replace Lua logging usage across modules with the new C++ logger; ensure log files rotate or truncate like the Lua version.
- Port `debug_utils.lua` only if needed; otherwise, provide equivalent conditional compilation hooks to trace behaviour during development builds.
- Document how to enable verbose tracing via schema options or environment variables.

## Phase 6 – Schema & Deployment Integration
- Update relevant `.schema.yaml` files to reference the new C++ components, retaining Lua fallbacks (commented or optional) for rollback while testing.
- Provide migration notes for users (configuration flags, build steps, troubleshooting).
- Coordinate with CI or packaging scripts to ensure the new plugin is built and distributed by default.

## Validation & Rollout Checklist
- [ ] All migrated modules compile without warnings and pass formatter/lint checks.
- [ ] Behavioural parity verified against Lua baseline for key workflows (AI chat triggers, raw English mode, punctuation filter).
- [ ] Logging and configuration reloads validated in long-running sessions.
- [ ] Fail-safe behaviour confirmed when cloud services are unreachable (graceful degradation).
- [ ] Documentation and schema updates reviewed and tested in a staging environment.

## Backlog / Stretch Goals
- Benchmark C++ vs Lua performance to quantify improvements.
- Explore asynchronous networking (libuv or std::async) for AI calls to avoid blocking the main thread.
- Add unit-test coverage for error conditions (config missing, malformed triggers).
- Investigate consolidating cloud-related code with existing Rime components to reduce duplication.
