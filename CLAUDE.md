# Working on this project

## Who you're working with

The owner is not a developer. He does not use git or GitHub, does not read
diffs, and should never be asked to. He builds this game by putting the headset
on and telling you how it feels. That is his job. Everything else is yours.

Concretely, **never**:

- ask him to merge, rebase, review, approve, or check a branch
- ask him to run a command, open a terminal, or look at a file
- explain what a draft PR, a base branch, or a merge conflict is and wait for a decision
- report something as "ready to merge" and leave it sitting there

If a task needs a git or GitHub action to be finished, do the action. Reporting
that it *could* be done is not finishing it.

The one thing only he can do is tell you whether it feels right in the headset.
Ask him that, and only that.

## Ship it — don't strand it

Work that isn't on `main` doesn't exist. This repo once had 15,375 lines of
finished, CI-green game sitting in a draft PR for four days while `main` stayed
a stub, because a draft PR cannot be merged and nobody said the magic word.
Don't recreate that.

- **Branch from `main`. Always.** Never open a PR whose base is another PR's
  branch. Stacking has already cost this project an entire finished enemy
  implementation that had to be deleted because it collided with work it was
  stacked on.
- **When CI goes green, merge it.** Don't wait to be asked. If a PR is open,
  green, and does what was asked, it should be on `main` the same session.
- **Open PRs ready for review, not as drafts.** If one does end up as a draft,
  take it out of draft before merging — that is a one-call fix, not a blocker
  to report upward.
- **Merge, don't squash.** The build-numbered commits are this project's
  narrative. Keep them.

## Keep the record honest

PR descriptions and code comments here go stale fast, and a stale description
is worse than none — it tells the owner the thing he asked for wasn't built
when it was.

- Rewrite the PR description when the work invalidates it, before merging.
- When a comment says "this is the next job" and you then do that job, update
  the comment.
- Don't describe intent as if it were achievement. If the headset hasn't judged
  it yet, say so.

## How to verify before it reaches him

Nothing goes to the headset unverified. The rule that produced this codebase:
*machine-check everything except feel.*

```bash
node wasm/test.js                    # engine suite — physics, mech IK, voxel damage
node wasm/page-test.js arena.html    # per-page: runs the real page headlessly
node wasm/page-test.js mech.html     #   with a scripted pilot, checks it draws
node wasm/page-test.js vox.html      #   and behaves. also: dummy, drag, handtrack
```

Both suites run in CI on every push. The four jobs are:

| job | what it proves |
|---|---|
| `apk` | the native Quest app actually compiles (Android NDK + CMake) |
| `wasm` | rebuilds `docs/box3d.wasm` from `wasm/bridge.c` and runs the engine suite |
| `pages` | every page still draws and behaves |
| `bench` | headless physics benchmark, for regression detection |

A page can crash on its first frame and look identical to a page that didn't
load — black, no error. That happened three times in a row here. The page
suite exists because of it; keep pages in it.

When you add a fix, add the check that fails without it. When a check is
measuring something known to be wrong, use `gap()` rather than deleting it or
loosening it until it passes — the number stays visible and honest.

## Where things are

```
docs/arena.html          the game — five-chapter campaign, WebXR
docs/*.html              single-purpose test pages (mech, vox, dummy, drag, handtrack)
docs/box3d.wasm          built artifact, rebuilt by CI from bridge.c — don't hand-edit
wasm/bridge.c            the engine: mech IK, voxel armour, enemy AI, campaign exports
wasm/build.md            how the wasm is built (Zig toolchain, pinned Box3D revision)
wasm/test.js             engine suite
wasm/page-test.js        page harness
app/src/main/cpp/        the native OpenXR Quest app (separate from the browser build)
docs/design/ROADMAP.md   the record: every system, measured number, and known limitation
```

The live game is the `gh-pages` branch, served at
<https://piedpiper80.github.io/Box3D-Quest-3/arena.html>. Deploying is a push to
that branch — there is no automation for it, so do it yourself and confirm the
deployed file matches what you built before telling him it's live.

## When you hand something over

Give him a link, and one specific question his body can answer. Not a status
report, not a checklist, not a request for approval. Something like:

> https://piedpiper80.github.io/Box3D-Quest-3/arena.html — hard-refresh.
> Does the heavy arm feel *heavy*, or just laggy? Those are different and only
> you can tell me which.

Anything about proportions, silhouette, or "does it look like X" — ask for a
reference image up front. Guessing at it costs whole rounds of headset time.
