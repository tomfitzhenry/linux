#!/usr/bin/env bash
# Unit and integration tests for loom/weave.sh and git-loom.
set -euo pipefail

WEAVE_SH="$(cd "$(dirname "$0")" && pwd)/weave.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export GIT_AUTHOR_NAME="Test Bot"
export GIT_AUTHOR_EMAIL="bot@example.com"
export GIT_COMMITTER_NAME="Test Bot"
export GIT_COMMITTER_EMAIL="bot@example.com"

echo "=== 1. Setting up synthetic kernel git repo ==="
git init -q -b master "$WORK/repo"
cd "$WORK/repo"
git config loom.changeId false

mkdir -p drivers/clk drivers/thermal arch/arm64/boot/dts
echo "clk-v1" > drivers/clk/clk.c
echo "thermal-v1" > drivers/thermal/thermal.c
echo "dts-v1" > arch/arm64/boot/dts/board.dts
git add .
git commit -q -m "Linux v7.4-rc2"
git tag v7.4-rc2

# Feature branch 1: soc/test/clk (will be merged upstream in v7.4-rc3)
git checkout -q -b soc/test/clk v7.4-rc2
echo "clk-v2-fix" >> drivers/clk/clk.c
git commit -q -a -m "clk: test: fix rate parent

Signed-off-by: Test Bot <bot@example.com>"

# Feature branch 2 + stacked branch 3: soc/test/thermal -> dts/test-board
git checkout -q -b soc/test/thermal v7.4-rc2
echo "thermal-v2" >> drivers/thermal/thermal.c
git commit -q -a -m "thermal: test: add sensor

Signed-off-by: Test Bot <bot@example.com>"

git checkout -q -b dts/test-board soc/test/thermal
echo "dts-v2-with-thermal" >> arch/arm64/boot/dts/board.dts
git commit -q -a -m "arm64: dts: add test board

Signed-off-by: Test Bot <bot@example.com>"

# local-ci branch
git checkout -q -b local-ci v7.4-rc2
mkdir -p loom
cp "$WEAVE_SH" loom/weave.sh
chmod +x loom/weave.sh
cat > loom/loom.toml << 'EOF'
base = "v7.4-rc2"
integration_branch = "lab/integration"

[[branch]]
name = "soc/test/clk"
upstream = true

[[branch]]
name = "soc/test/thermal"
upstream = true

[[branch]]
name = "dts/test-board"
stacked_on = "soc/test/thermal"
upstream = true

[[branch]]
name = "local-ci"
upstream = false
EOF
git add loom/
git commit -q -m "loom: initial manifest"

echo "=== 2. Testing weave.sh verify & apply ==="
./loom/weave.sh verify
./loom/weave.sh apply

git checkout -q lab/integration
grep -q "clk-v2-fix" drivers/clk/clk.c
grep -q "thermal-v2" drivers/thermal/thermal.c
grep -q "dts-v2-with-thermal" arch/arm64/boot/dts/board.dts

echo "=== 3. Testing missing Signed-off-by rejection ==="
git checkout -q -b bad/no-sob v7.4-rc2
echo "bad" > drivers/clk/bad.c
git add drivers/clk/bad.c
git commit -q -m "clk: missing sob"
cat > "$WORK/bad-sob.toml" << 'EOF'
base = "v7.4-rc2"
integration_branch = "deploy"
[[branch]]
name = "bad/no-sob"
upstream = true
EOF
if ./loom/weave.sh verify --manifest "$WORK/bad-sob.toml" 2>/dev/null; then
  echo "FAIL: expected missing Signed-off-by to fail verify" >&2
  exit 1
fi

echo "=== 4. Testing forbidden CI file on upstream branch rejection ==="
git checkout -q -b bad/touches-ci v7.4-rc2
echo "ci" > flake.nix
git add flake.nix
git commit -q -m "ci: bad

Signed-off-by: Test Bot <bot@example.com>"
cat > "$WORK/bad-ci.toml" << 'EOF'
base = "v7.4-rc2"
integration_branch = "deploy"
[[branch]]
name = "bad/touches-ci"
upstream = true
EOF
if ./loom/weave.sh verify --manifest "$WORK/bad-ci.toml" 2>/dev/null; then
  echo "FAIL: expected upstream branch touching flake.nix to fail verify" >&2
  exit 1
fi

echo "=== 5. Testing conflict detection ==="
git checkout -q -b bad/conflict v7.4-rc2
echo "clk-conflicting-change" >> drivers/clk/clk.c
git commit -q -a -m "clk: conflict

Signed-off-by: Test Bot <bot@example.com>"
if ./loom/weave.sh verify --manifest "$WORK/repo/loom/loom.toml" --extra-branch bad/conflict 2>/dev/null; then
  echo "FAIL: expected conflicting extra branch to fail verify" >&2
  exit 1
fi

echo "=== 6. Testing rebase with upstreamed branch auto-drop ==="
git checkout -q master
# Simulate Linus merging soc/test/clk upstream in v7.4-rc3!
echo "clk-v2-fix" >> drivers/clk/clk.c
git commit -q -a -m "clk: test: fix rate parent (merged upstream)"
echo "unrelated-upstream-change" > README
git add README
git commit -q -m "Linux v7.4-rc3"
git tag v7.4-rc3

git checkout -q local-ci
./loom/weave.sh rebase v7.4-rc3

# Verify soc/test/clk was automatically dropped from loom/loom.toml
if grep -q "soc/test/clk" loom/loom.toml; then
  echo "FAIL: expected upstreamed branch soc/test/clk to be dropped from loom.toml" >&2
  exit 1
fi
grep -q 'base = "v7.4-rc3"' loom/loom.toml

# Verify lab/integration is now based on v7.4-rc3 and contains all changes
git checkout -q lab/integration
git merge-base --is-ancestor v7.4-rc3 lab/integration
grep -q "unrelated-upstream-change" README
grep -q "clk-v2-fix" drivers/clk/clk.c
grep -q "thermal-v2" drivers/thermal/thermal.c
grep -q "dts-v2-with-thermal" arch/arm64/boot/dts/board.dts

echo "ALL LOOM TESTS PASSED"
