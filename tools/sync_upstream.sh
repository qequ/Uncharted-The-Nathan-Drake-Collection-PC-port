#!/usr/bin/env bash
# Brings new commits from the port this fork follows (remote "upstream", bc160201983's
# Uncharted-The-Nathan-Drake-Collection-PC-port) into our branch. This checkout stays untouched
# until the merge builds and passes the tests:
#   1. fetches upstream, and stops when our branch already has all of it;
#   2. merges upstream/main into the branch "sync" in a separate worktree (UNCHARTED_SYNC_DIR,
#      default ../Uncharted-sync; made on first use, with local copies of the submodules);
#   3. builds there and runs the tests (gow3_pc/build.sh --test, the Python unit tests: only
#      failures the merge brought count).
# A merge conflict stops at step 2: resolve it in that worktree, commit, and run this again.
# Play-test from the worktree (py tools/probe_uncharted.py ... started there), then
#   bash tools/sync_upstream.sh --apply
# fast-forwards this checkout's branch to "sync" (git refuses if uncommitted edits here touch the
# same files), sets "main" to upstream's, and pushes both and upstream's tags to origin.
set -euo pipefail
main_root=$(git -C "$(dirname -- "$0")" worktree list --porcelain | sed -n '1s/^worktree //p')
cd -- "$main_root"
branch=$(git symbolic-ref --short HEAD)
sync_dir=${UNCHARTED_SYNC_DIR:-$(dirname -- "$main_root")/Uncharted-sync}
python=${PYTHON:-$(command -v py || command -v python)}
# The FSR-Vulkan patches leave that submodule modified: only commits count as changes.
clean() { git -C "$1" diff --quiet --ignore-submodules=dirty HEAD --; }

if [[ ${1:-} == --apply ]]; then
    clean "$sync_dir" || { echo "$sync_dir has uncommitted changes" >&2; exit 1; }
    git merge-base --is-ancestor upstream/main sync || { echo "sync lacks upstream/main: run without --apply first" >&2; exit 1; }
    git merge --ff-only sync || { echo "$branch has commits sync lacks: run without --apply first" >&2; exit 1; }
    git branch -f main upstream/main
    # The GitHub CLI supplies the credentials when it is installed and logged in.
    gh=$(command -v gh || echo "/c/Program Files/GitHub CLI/gh.exe")
    push=(git)
    if [[ -x $gh ]] && "$gh" auth status >/dev/null 2>&1; then
        push+=(-c credential.helper= -c "credential.helper=!\"$gh\" auth git-credential")
    fi
    "${push[@]}" push origin "$branch" main $(git tag --merged upstream/main | sed 's#^#refs/tags/#')
    echo "Pushed $branch ($(git rev-parse --short HEAD)) and main. Rebuild here: bash gow3_pc/build.sh"
    exit 0
fi

git fetch -q upstream --tags --prune
if git merge-base --is-ancestor upstream/main "$branch"; then
    echo "Up to date: $branch has upstream/main ($(git rev-parse --short upstream/main))."
    exit 0
fi
echo "New upstream commits:"
git log --format='  %h %ad %s' --date=short "$branch..upstream/main"

submodules=$(git config -f .gitmodules --get-regexp 'submodule\..*\.path' | awk '{print $2}')
if [[ ! -e $sync_dir ]]; then
    git worktree prune
    git worktree add -q -B sync "$sync_dir" "$branch"
fi
cd -- "$sync_dir"
clean . || { echo "$sync_dir has uncommitted changes: commit or discard them first" >&2; exit 1; }
git checkout -q sync
git merge -q --no-edit "$branch"
python_failures() {
    (cd gow3_pc && "$python" -m unittest discover -s tests 2>&1 || true) |
        sed -n 's/^\(ERROR\|FAIL\): \(.*\)$/\2/p' | sort
}
before=$(python_failures)
if ! git merge --no-edit -m "Merge upstream $(git describe --tags --abbrev=0 upstream/main 2>/dev/null || echo main) ($(git rev-parse --short upstream/main))" upstream/main; then
    echo "Merge conflict in $sync_dir:" >&2
    git diff --name-only --diff-filter=U >&2
    echo "Resolve it there, commit, and run this again." >&2
    exit 1
fi
# Submodules: local clones of this checkout's submodule repositories (or of their URL), checked
# out with the line endings the FSR-Vulkan patches need (core.autocrlf false, as here).
for path in $submodules; do
    want=$(git rev-parse "HEAD:$path")
    url=$(git config -f .gitmodules "submodule.$path.url")
    if [[ ! -e $path/.git ]]; then
        rm -rf -- "$path"
        if [[ -d $main_root/.git/modules/$path ]]; then
            git clone -q --shared --no-checkout "$main_root/.git/modules/$path" "$path"
        else
            git clone -q --no-checkout "$url" "$path"
        fi
        git -C "$path" config core.autocrlf false
    elif [[ $(git -C "$path" rev-parse HEAD) == "$want" ]]; then
        continue
    fi
    git -C "$path" cat-file -e "$want^{commit}" 2>/dev/null || git -C "$path" fetch -q "$url" "$want"
    git -C "$path" checkout -q -f "$want"
done

bash gow3_pc/build.sh --test
after=$(python_failures)
new=$(comm -13 <(echo "$before") <(echo "$after"))
if [[ -n $new ]]; then
    echo "Python tests that fail only after the merge:" >&2
    echo "$new" | sed 's/^/  /' >&2
    exit 1
fi
echo
echo "Merged, built and tested in $sync_dir (branch sync, $(git rev-parse --short HEAD))."
echo "Play-test from there, then: bash tools/sync_upstream.sh --apply"
