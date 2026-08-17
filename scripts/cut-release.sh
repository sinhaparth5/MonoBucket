#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cut a MonoBucket release.
#
#   scripts/cut-release.sh            # next CalVer for today's month
#   scripts/cut-release.sh 2026.09.0  # an explicit version
#   scripts/cut-release.sh --dry-run  # show what would change
#
# Does, in order: derive the version, rewrite CMakeLists.txt, move the
# [Unreleased] changelog block under a dated heading, commit, and tag.
#
# It exists because release.yml refuses to publish when the tag, CMakeLists.txt
# and CHANGELOG.md disagree, and doing those three by hand in the right order is
# exactly the kind of ritual that fails at 23:00 on the day it matters. MICRO
# resetting when the month rolls over is the part nobody remembers.
# ---------------------------------------------------------------------------
set -euo pipefail

cd "$(dirname "$0")/.."

DRY_RUN=false
VERSION=""
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)  VERSION="$arg" ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }

# --- Derive the version ----------------------------------------------------

YEAR="$(date -u +%Y)"
MONTH="$(date -u +%m)"

if [ -z "$VERSION" ]; then
    # MICRO counts releases within this month and restarts at 0 when the month
    # does, so it comes from the tags that already exist rather than from the
    # current value in CMakeLists.txt.
    LAST="$(git tag -l "v$YEAR.$MONTH.*" \
            | sed "s/^v$YEAR\.$MONTH\.//" \
            | grep -E '^[0-9]+$' \
            | sort -n | tail -1 || true)"
    if [ -z "$LAST" ]; then
        MICRO=0
    else
        MICRO=$((LAST + 1))
    fi
    VERSION="$YEAR.$MONTH.$MICRO"
else
    [[ "$VERSION" =~ ^([0-9]{4})\.([0-9]{2})\.([0-9]+)(-[0-9A-Za-z.-]+)?$ ]] \
        || die "'$VERSION' is not CalVer YYYY.0M.MICRO[-prerelease]"
    YEAR="${BASH_REMATCH[1]}"
    MONTH="${BASH_REMATCH[2]}"
    MICRO="${BASH_REMATCH[3]}"
fi

MONTH_INT="$((10#$MONTH))"
TAG="v$VERSION"
TODAY="$(date -u +%Y-%m-%d)"

echo "Cutting $VERSION ($TAG), dated $TODAY"

# --- Refuse to release from a state nobody can reproduce -------------------

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    die "tag $TAG already exists"
fi

if [ -n "$(git status --porcelain)" ] && [ "$DRY_RUN" = false ]; then
    die "working tree is dirty; commit or stash first"
fi

if ! grep -q '^## \[Unreleased\]' CHANGELOG.md; then
    die "CHANGELOG.md has no [Unreleased] section to promote"
fi

# An [Unreleased] block with no entries under it means the release notes would
# be empty, which is worse than no release.
if ! awk '/^## \[Unreleased\]/{c=1;next} c && /^## \[/{exit} c && /^- |^### /{found=1} END{exit !found}' CHANGELOG.md; then
    die "[Unreleased] is empty — nothing to release"
fi

# --- Rewrite CMakeLists.txt ------------------------------------------------

cmake_edit() {
    sed -i \
        -e "s/^set(MONOBUCKET_VERSION_YEAR  .*/set(MONOBUCKET_VERSION_YEAR  $YEAR)/" \
        -e "s/^set(MONOBUCKET_VERSION_MONTH .*/set(MONOBUCKET_VERSION_MONTH $MONTH)/" \
        -e "s/^set(MONOBUCKET_VERSION_MICRO .*/set(MONOBUCKET_VERSION_MICRO $MICRO)/" \
        -e "s/^        VERSION .*/        VERSION $YEAR.$MONTH_INT.$MICRO/" \
        CMakeLists.txt
}

# --- Rewrite CHANGELOG.md --------------------------------------------------
#
# The promoted block keeps its entries; a fresh, empty [Unreleased] takes its
# place so the next change has somewhere to land.

changelog_edit() {
    awk -v ver="$VERSION" -v today="$TODAY" '
        /^## \[Unreleased\]/ {
            print "## [Unreleased]"
            print ""
            print "Nothing yet."
            print ""
            print "---"
            print ""
            print "## [" ver "] — " today
            next
        }
        { print }
    ' CHANGELOG.md > CHANGELOG.md.new
    mv CHANGELOG.md.new CHANGELOG.md
}

if [ "$DRY_RUN" = true ]; then
    echo
    echo "--- CMakeLists.txt would become ---"
    cmake_edit
    grep -n 'MONOBUCKET_VERSION_\(YEAR\|MONTH\|MICRO\)\|^        VERSION' CMakeLists.txt
    git checkout -- CMakeLists.txt
    echo
    echo "--- CHANGELOG.md heading would become ---"
    echo "## [$VERSION] — $TODAY"
    echo
    echo "--- then ---"
    echo "git commit -m 'Release $VERSION' && git tag -a $TAG"
    exit 0
fi

cmake_edit
changelog_edit

# The version the build reports has to match the tag, and CMake is the only
# thing that knows how it assembles it. Cheaper to ask than to assume.
if ! grep -q "^set(MONOBUCKET_VERSION_MICRO $MICRO)$" CMakeLists.txt; then
    die "CMakeLists.txt did not take the version rewrite; check its formatting"
fi

git add CMakeLists.txt CHANGELOG.md ROADMAP.md
git commit -m "Release $VERSION"
git tag -a "$TAG" -m "MonoBucket $VERSION"

cat <<EOF

Tagged $TAG.

Review it, then publish:

    git push origin master
    git push origin $TAG

The tag push builds linux/amd64 and linux/arm64 on native runners, smoke-tests
each, signs the manifest with cosign and creates the GitHub release from the
'## [$VERSION]' section of CHANGELOG.md.
EOF
