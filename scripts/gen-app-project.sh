#!/usr/bin/env bash
# gen-app-project.sh — (copied from GoldenEye-Decomp-ios 55a3a9c8, round 9b;
# the version files live in app/ here, not at the repo root) substitute the version tokens into app/project.yml and
# run xcodegen. Every build goes through this; nothing calls `xcodegen generate`
# on project.yml directly any more.
#
#   scripts/gen-app-project.sh          # marketing version from VERSION/DEV_ITERATION
#
# Why a generated spec at all (PD's gen-app-project.sh, same shape): the
# marketing version has exactly ONE source of truth in this repo — the VERSION
# and DEV_ITERATION files (~/dev/CLAUDE.md §Versioning: pre-first-release OTA
# builds are 0.0.0.1, 0.0.0.2, …). A literal in project.yml is a second one, and
# the two drift the first time someone bumps only one of them. app/project.yml
# therefore carries the tokens GE_MARKETING_VERSION and GE_BUILD_NUMBER and is
# not, by itself, a buildable spec.
#
# GE_BUILD_NUMBER (CFBundleVersion) is a UTC minute stamp: it churns per build
# and is not a version, it is "which build is this". publish-ota.sh passes its
# own so the archive and the hub row agree.
#
# app/project.generated.yml and app/ProjectMidas.xcodeproj are both gitignored.
#
# Both targets come out of the one spec (round 30): ProjectMidas (iOS) and
# ProjectMidas-visionos, each with its own scheme; the version tokens are
# substituted into both, so iOS and visionOS builds always carry one number.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PUBVER="$(tr -d ' \n' < app/VERSION)"
DEVIT="$(tr -d ' \n' < app/DEV_ITERATION)"
[ -n "$PUBVER" ] || { echo "FATAL: VERSION is empty" >&2; exit 1; }
[ -n "$DEVIT" ]  || { echo "FATAL: DEV_ITERATION is empty" >&2; exit 1; }
if [ "$DEVIT" = "0" ]; then MARKETING="$PUBVER"; else MARKETING="$PUBVER.$DEVIT"; fi
BUILDNUM="${GE_BUILD_NUMBER:-$(date -u +%Y%m%d%H%M)}"

command -v xcodegen >/dev/null || { echo "FATAL: xcodegen not installed (brew install xcodegen)" >&2; exit 1; }

sed -e "s/GE_MARKETING_VERSION/$MARKETING/g" \
    -e "s/GE_BUILD_NUMBER/$BUILDNUM/g" \
    app/project.yml > app/project.generated.yml

grep -q "GE_MARKETING_VERSION\|GE_BUILD_NUMBER" app/project.generated.yml \
	&& { echo "FATAL: a version token survived substitution" >&2; exit 1; } || true

( cd app && xcodegen generate --quiet --spec project.generated.yml )
echo "project generated: version $MARKETING, build $BUILDNUM"
