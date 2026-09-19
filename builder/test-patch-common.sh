#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/patch-common.sh"
fixture="$(mktemp -d /tmp/melee-patch-test.XXXXXXXX)"
cleanup() {
  case "$fixture" in /tmp/melee-patch-test.*) rm -rf -- "$fixture" ;; esac
}
trap cleanup EXIT
git init -q "$fixture"
mkdir "$fixture/nested"
printf 'before\n' > "$fixture/nested/value.txt"
cat > "$fixture/change.patch" <<'PATCH'
diff --git a/value.txt b/value.txt
--- a/value.txt
+++ b/value.txt
@@ -1 +1 @@
-before
+after
PATCH
# A reverse check must fail on untouched files (the skipped-hunk regression).
if git_apply_tree "$fixture/nested" --reverse --check "$fixture/change.patch" 2>/dev/null; then
  echo 'ERROR: unmodified nested tree passed reverse check' >&2
  exit 1
fi
git_apply_tree "$fixture/nested" --check "$fixture/change.patch"
git_apply_tree "$fixture/nested" "$fixture/change.patch"
[[ "$(cat "$fixture/nested/value.txt")" == after ]]
git_apply_tree "$fixture/nested" --reverse --check "$fixture/change.patch"
# Check ordinary unified patches too; Git treats their prefixes differently
# when run from a subdirectory, which was hidden by the old checks.
tail -n +2 "$fixture/change.patch" > "$fixture/unified.patch"
git_apply_tree "$fixture/nested" --reverse --check "$fixture/unified.patch"
git_apply_tree "$fixture/nested" --reverse "$fixture/unified.patch"
[[ "$(cat "$fixture/nested/value.txt")" == before ]]
git init -q "$fixture/nested"
git_apply_tree "$fixture/nested" "$fixture/change.patch"
git_apply_tree "$fixture/nested" --reverse --check "$fixture/change.patch"
echo 'Patch helper: nested Git-format, nested unified, and standalone trees passed'
