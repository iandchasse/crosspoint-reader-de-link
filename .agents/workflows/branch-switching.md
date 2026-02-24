---
description: how to switch between s3-port work and upstream PR work
---

# Branch + Submodule Workflow

This repo has two parallel tracks that each need a different `open-x4-sdk` commit.
The `.git/hooks/post-checkout` hook handles switching automatically.

---

## How it works

Each branch records a specific `open-x4-sdk` commit SHA in the git tree.
When you switch branches, the hook runs `git submodule update` to sync the
submodule working tree to the commit that branch recorded.

| Branch | Submodule commit |
|---|---|
| `s3-port` | `4380748` — `s3-port` of open-x4-sdk (has `EspFsFile`) |
| any upstream branch | `9f76376` — upstream master of open-x4-sdk (has `FsFile`) |

---

## Switching between tracks

### → S3 work (`s3-port`)

```sh
git checkout s3-port
# [post-checkout] Syncing submodules for s3-port...
# open-x4-sdk → 4380748 (s3-port, EspFsFile)
```

### → Upstream PR work

```sh
git fetch upstream
git checkout -b feature/my-fix upstream/master
# or just:
git checkout refactor/gfx-wrapped-text
# [post-checkout] Syncing submodules for refactor/gfx-wrapped-text...
# open-x4-sdk → 9f76376 (upstream master, FsFile)
```

**Wait for the hook to print `Submodule sync complete.` before building.**

---

## If you have dirty/uncommitted changes in the submodule

The hook will **automatically stash** them before syncing:

```
[post-checkout] Stashing dirty submodule: open-x4-sdk
[post-checkout] Submodule sync complete.
```

To restore them after switching back:

```sh
git -C open-x4-sdk stash pop
```

---

## If the main-repo branch itself has dirty files

Git will refuse the checkout if tracked files would be overwritten.
The usual case is `lib/I18n/I18nKeys.h` diverging between branches.

```sh
# Stash just the conflicting files, then switch
git stash -- lib/I18n/I18nKeys.h lib/I18n/I18nStrings.h
git checkout <target-branch>
# restore if you need them later:
git stash pop
```

---

## Starting a new upstream PR branch

```sh
git fetch upstream
git checkout -b feature/my-new-fix upstream/master
# hook syncs open-x4-sdk to upstream master automatically
# ... make changes ...
git push origin feature/my-new-fix
# open PR against crosspoint-reader/crosspoint-reader master
```

---

## Starting a new S3 feature branch

```sh
git checkout -b feature/my-s3-thing s3-port
# hook syncs open-x4-sdk to s3-port SDK commit automatically
# ... make changes ...
git push origin feature/my-s3-thing
# open PR against your fork's s3-port branch (NOT upstream master)
```

---

## Pinning a new submodule commit to a branch

If you update open-x4-sdk and want your branch to always use the new commit:

```sh
cd open-x4-sdk
# make / pull your changes, commit them
git commit -am "my sdk change"
cd ..
git add open-x4-sdk
git commit -m "chore: update open-x4-sdk pointer"
# future checkouts to this branch will automatically land on the new commit
```
