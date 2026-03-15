# ViperLang LLM Pack

This folder is the onboarding pack for LLMs and agentic coding systems.

Read order:

1. `MODEL_SPEC.md`
2. `STDLIB.md`
3. `ERRORS.md`
4. `TOOLING.md`
5. `BUILD.md`
6. `PACKAGING.md`
7. `TESTING.md`
8. `RECIPES.md`
9. `WORKFLOW.md`
10. `PROMPTS.md`

If a model can only receive one file, use:

- `docs/llm/MODEL_SPEC.md`

If a model can receive four files, use:

- `docs/llm/MODEL_SPEC.md`
- `docs/llm/STDLIB.md`
- `docs/llm/ERRORS.md`
- `docs/llm/TOOLING.md`

The intent of this pack is not just to teach syntax. It teaches the model:

- what ViperLang is optimized for
- how to write idiomatic Viper
- how to avoid current parser/runtime pitfalls
- how to use Viper's semantic state tools instead of reading full source
- which stdlib modules really exist today
- how package, ABI, build, and capability flows work
- how test linkage and proof sidecars work
- what application shapes fit Viper best
- how to resolve failures by stable error code instead of rereading long logs
