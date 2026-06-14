#!/usr/bin/env bash
# Start a new mod-dungeon-clear feature in an ISOLATED git worktree branched
# from master. One feature = one worktree = one branch. Changes in one feature
# can never bleed into another's working tree.
#
#   tools/dc-feature.sh <feature-name> [feat|fix|refactor|tune|perf|test]
#
# Then work inside the printed directory, committing as you go. When done:
#   git merge --no-ff <branch>
#   git worktree remove <dir>
#   git branch -d <branch>
set -euo pipefail

name="${1:?usage: dc-feature.sh <feature-name> [kind]}"
kind="${2:-feat}"
repo="$(git rev-parse --show-toplevel)"
branch="${kind}/${name}"
wt="${repo}/.claude/worktrees/${name}"

if [ -e "$wt" ]; then
  echo "error: worktree already exists: $wt" >&2
  exit 1
fi

git -C "$repo" fetch gh master --quiet 2>/dev/null || true
git -C "$repo" worktree add -b "$branch" "$wt" master

echo
echo "Worktree ready:  $wt"
echo "Branch:          $branch  (forked from master)"
echo
echo "When the feature is finished and committed:"
echo "  git -C '$repo' merge --no-ff $branch"
echo "  git -C '$repo' worktree remove '$wt'"
echo "  git -C '$repo' branch -d $branch"
