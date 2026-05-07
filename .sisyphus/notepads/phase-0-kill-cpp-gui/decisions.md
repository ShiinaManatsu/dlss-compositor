# Decisions — phase-0-kill-cpp-gui

## [2026-05-07] Plan execution decisions

- Use `git rm` for file deletions (not OS delete) to preserve git history
- Wave 1 tasks (2-6) run in parallel after Task 1 gate passes
- All Wave 1 changes committed together in a single atomic commit after Task 7 passes
- No worktree used — working directly in main repo
