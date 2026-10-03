# Agent instructions

- Always commit completed changes in this repository. After verification, stage the task's files, create a descriptive commit, and confirm the working tree status before reporting completion. Do not leave finished work uncommitted.
- Add new functionality on a branch and land it through a pull request, never by committing to `main`. Before merging, rebase the branch onto the current `main` (no merge commits from `main` into the branch) and have a green CI run on the rebased head. Do not merge on a red or still-running run; see "Branches and pull requests" in CONTRIBUTING.md for the steps.
