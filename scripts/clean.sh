#!/usr/bin/env bash
set -euo pipefail

# Patterns/paths to consider for removal (adjust as needed)
readonly PATTERNS=(
    "build"
    "dist"
    "node_modules"
    ".vscode"
    ".idea"
    ".cache"
    "__pycache__"
    "*.o"
    "*~"
    "*.bak"
    "*.pyc"
    ".DS_Store"
    "*.swp"
    "sysd-mgr"    # built binary
    "core"
)

DRY_RUN=1
if [[ "${1:-}" == "--yes" || "${1:-}" == "-y" ]]; then
    DRY_RUN=0
fi

echo "Project cleanup script"
echo "Project root: $(pwd)"
echo

# Show untracked/ignored preview from git (if repository)
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "-- git untracked/ignored preview --"
    git clean -ndX || true
    echo
fi

echo "-- Files/dirs that match configured patterns (dry-run mode: not deleting) --"
for pat in "${PATTERNS[@]}"; do
    # find up to reasonable depth to avoid scanning huge trees; adjust -maxdepth if needed
    find . -maxdepth 4 -name "$pat" -print 2>/dev/null || true
done
echo

if (( DRY_RUN )); then
    echo "Dry run complete. To actually delete the above items run:"
    echo "  ./scripts/clean.sh --yes"
    echo "Or run the git removal (dangerous) preview -> apply with:"
    echo "  git clean -fdX   # removes untracked + ignored files"
    exit 0
fi

echo "Removing matched files/dirs..."
for pat in "${PATTERNS[@]}"; do
    # use -exec rm -rf to remove all matches
    find . -maxdepth 4 -name "$pat" -print -exec rm -rf {} + 2>/dev/null || true
done

# If repo, ask/perform git clean for untracked files not matched above
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo
    echo "Running git clean -fdX to remove remaining untracked/ignored files (confirmed mode)..."
    git clean -fdX
fi

echo "Cleanup finished."