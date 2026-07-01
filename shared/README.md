# shared/

Workspace-wide headers every SoWMods mod compiles against via the CMake include
`${CMAKE_CURRENT_SOURCE_DIR}/../shared`.

- **GameOffsets.h** — the single source of truth for all hand-found RVAs (`namespace game`,
  off image base `0x140000000`). Each mod's `src/Offsets.h` is the only file that includes it and
  re-exports the `k`-names it needs.

⚠ **Not its own git repo yet.** The Stop-hook auto-commit only commits sibling dirs that have `.git`,
so edits here are **local-only** until we decide how to version it (own repo `Hagryph/SoWShared`
added to the hook, a git submodule per mod, or vendored copies). A fresh clone of any mod repo will
be missing `shared/`.
