# Keeping the AMD fork current

`origin` is `https://github.com/burnet01/SplatPC-Splatoon-AMD.git`.
`upstream` is `https://github.com/PC-PORTERS/SplatPC-Splatoon-Native-PC-Port.git`.

The original local snapshot is preserved at
`backup/amd-before-upstream-2026-11-03` (`d89238c`). The installed game is ignored
by Git and is not replaced by a source merge.

## Recovering the shared history

The local repository started with a snapshot instead of upstream's commit
history. A direct unrelated-history merge would treat existing source files as
independent additions. Comparing upstream revisions identified `b928f51` as the
matching baseline before the Amiibo and performance changes.

`archive/local-changes-against-upstream` records the original snapshot tree with
that baseline as its parent. Its changes were applied to upstream `dde4e71`
using a three-way merge. Both the new vertex-cache optimization and the AMD
stride-padding requirement are retained. Local GPU selection, shader defaults,
validation fixes, bots, tests, and platform files are also retained.

The integration also joins the original snapshot history, so subsequent updates
use ordinary Git merges and do not require reconstructing the baseline again.

## Future updates

Start with a clean working tree and preserve a branch before updating:

```sh
git switch main
git branch backup/before-next-upstream-update
git fetch upstream
git switch -c integration/next-upstream-update
git merge upstream/main
```

Resolve conflicts by preserving the relevant behavior from both sides, then
build and run the GPU regressions before merging the integration branch into
`main`. Recompiler or runtime-header changes also require rebuilding generated
guest objects; an old game executable does not validate those changes.

```sh
git switch main
git merge --ff-only integration/next-upstream-update
git push origin main
```

The push publishes the reviewed changes and is a separate explicit step. Never
force-push over upstream history to synchronize this fork. Game dumps, generated
guest code, binaries, saves, and caches remain local and ignored.
