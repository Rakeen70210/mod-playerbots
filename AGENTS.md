# Repository Guidelines

## Project Structure & Module Organization

- `src/`: C++ implementation of the Playerbots module (AI, managers, DB helpers, scripts, utilities).
- `conf/`: module configuration templates (notably `conf/playerbots.conf.dist`).
- `data/sql/`: SQL for `world`, `characters`, and `playerbots` databases:
  - `base/` for initial schema/data
  - `updates/` for incremental migrations (date-prefixed files like `YYYY_MM_DD_NN_*.sql`)
- `apps/`:
  - `apps/codestyle/`: lightweight C++ style checks
  - `apps/vector_memory_service/`: optional Python service used by route/quest “vector memory”
- Repo scripts: `code_format.sh`, `setup_git_commit_template.sh`, `include.sh`.

## Build, Test, and Development Commands

This repo is intended to be built as an **AzerothCore module** (no standalone `CMakeLists.txt` here). Build from the
required AzerothCore `Playerbot` branch checkout:

```bash
# from azerothcore-wotlk/
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Formatting and local checks:

- `./code_format.sh`: runs `clang-format -i` over `*.cpp`/`*.h` using `.clang-format`.
- `python3 apps/codestyle/codestyle-cpp.py`: basic codestyle checks aligned with AzerothCore standards.

Optional vector memory service:
- See `apps/vector_memory_service/README.md` (Qdrant + `uvicorn main:app --port 7788`).
- C++ integration lives in `src/Util/VectorMemoryMgr.cpp` and is used by `src/Mgr/Travel/TravelMgr.cpp`,
  `src/Ai/Base/Actions/MoveToTravelTargetAction.cpp`, and `src/Ai/World/Rpg/Action/NewRpgBaseAction.cpp`.
- Enable via `AiPlayerbot.VectorMemoryEnabled = 1` and the other `AiPlayerbot.VectorMemory*` settings in
  `conf/playerbots.conf.dist`.

## Coding Style & Naming Conventions

- Indentation: 4 spaces (`.editorconfig`).
- Formatting: `clang-format` with Allman braces and 120-column limit (`.clang-format`).
- Follow AzerothCore C++ code standards and existing patterns in nearby code.
- SQL: prefer `InnoDB` for dynamic tables; keep migrations additive and place them under the correct DB folder in
  `data/sql/*/updates/`.

## Testing Guidelines

There is no dedicated unit-test harness in this repo; verification is primarily:

1. Compile worldserver with the module enabled.
2. Run targeted in-game scenarios (commands, random bots, raids/BGs as relevant).
3. Confirm defaults remain cheap and heavy logic is opt-in where applicable.

## Commit & Pull Request Guidelines

- Commit messages: use the conventional template in `.git_commit_template.txt`:
  - Run `./setup_git_commit_template.sh`
  - Prefer `fix(CORE/...): ...`, `fix(DB/...): ...`, `feat(...)`, etc.
- PRs: follow `PULL_REQUEST_TEMPLATE.md` (testing steps, performance/scaling impact, defaults/config, and AI-assistance
  disclosure when applicable).
