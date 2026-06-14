# mod-dungeon-clear — development workflow

These rules exist because uncommitted work was getting orphaned across sessions
and feature branches got stacked on each other, forcing painful cherry-pick
untangling. Follow them exactly.

## The five rules

1. **Always branch from `master`, never from another feature branch.**
   `git switch -c feat/<name> master`. Stacking a feature on top of another
   unmerged feature is what forced the ZF / Sunken-Temple cherry-pick rewrite.
   Only branch off another feature branch when the dependency is deliberate and
   stated.

2. **One feature = one isolated worktree.** When more than one feature is in
   flight, give each its own working tree so their uncommitted files can never
   mix. Use `tools/dc-feature.sh <name> [feat|fix|refactor|...]` — it creates
   `.claude/worktrees/<name>` on a fresh master-based branch.

3. **A session boundary is a commit boundary — never stop on a dirty tree.**
   Before ending a session, commit, even if the feature is half-done:
   `git commit -m "wip: <what is done / what remains>"` on the feature branch.
   A half-finished feature is a *named branch with a wip commit*, never loose
   files in the working tree.

4. **Stamp commits with the originating session id** as a trailer, so any change
   can be traced back to the session that produced it:
   ```
   Session: S557
   ```

5. **Delete a feature branch the moment it merges.**
   `git merge --no-ff feat/<name> && git branch -d feat/<name>`. Keep the branch
   list to *only things in flight*. Periodically sweep:
   `git branch --merged master | grep -vE '^\*|master$' | xargs -r git branch -d`.

## Branch naming
`feat/` `fix/` `refactor/` `tune/` `perf/` `test/` `diag/` — prefix matches the
nature of the change.

## Intentionally-preserved unmerged branches
These diverge from master on purpose; do **not** delete them:
- `feat/cinematic-camera` — abandoned approach, history kept for reference.
- `fix/pause-leader-follow-hold`, `fix/pause-toggle-autopause-race` — reverted
  off master, preserved for re-diagnosis if the pause bug resurfaces.
- `refactor/consolidate-party-combat-state` — genuine WIP.

## Session start / end safety net
A `SessionStart` hook prints this module's git state (dirty tree + in-flight
branches) at the top of every session; a `Stop` hook warns if you try to end a
session with a dirty tree. They are reminders, not a substitute for the rules.

## Build & test
The user always builds and deploys — do not build or inspect the binary.
Run the gtest suite from this module root: `sudo bash t/run_tests.sh`.
Plan / design / review docs live in `deployment-files/docs/`, never committed
to this repo.
