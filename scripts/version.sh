#!/usr/bin/env bash
#
# version.sh - Update version numbers across the module
#
# Usage: ./scripts/version.sh X.X.X
# Example: ./scripts/version.sh 0.2.0
#
# Updates version in:
#   - src/module.json ("version": "X.X.X")
#
# Does NOT perform any git actions (tag, commit, push).
#

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 X.X.X"
    echo "Example: $0 0.2.0"
    exit 1
fi

VERSION="$1"

# Validate version format
if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Error: Version must be in X.X.X format (e.g. 0.2.0)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Updating version to $VERSION..."

# Update module.json
MODULE_JSON="$ROOT_DIR/src/module.json"
if [ -f "$MODULE_JSON" ]; then
    sed -i "s/\"version\": \"[0-9]*\.[0-9]*\.[0-9]*\"/\"version\": \"$VERSION\"/" "$MODULE_JSON"
    echo "  Updated: src/module.json"
else
    echo "  Warning: src/module.json not found"
fi

echo "Done. Version is now $VERSION"
echo ""
echo "Next steps (manual):"
echo "  git add -A"
echo "  git commit -m \"Release v$VERSION\""
echo "  git tag v$VERSION"
echo "  git push && git push --tags"
