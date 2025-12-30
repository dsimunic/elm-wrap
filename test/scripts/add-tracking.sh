#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

# Add since-count tracking to all test functions (no .sh extension files)

SCRIPTS_DIR="/Volumes/Devel/Projects/elm-wrap/test/scripts/install-uninstall"

# Process all test-* scripts
for script in "$SCRIPTS_DIR"/test-*; do
    # Skip if not executable or not a regular file
    [[ -f "$script" && -x "$script" ]] || continue
    
    # Skip if already has since tracking
    if grep -q 'since_before=$(get_v1_since_count)' "$script"; then
        echo "Skipping $(basename "$script") - already has tracking"
        continue
    fi
    
    echo "Processing $(basename "$script")..."
    
    # Manual approach - will handle this per script in the actual modifications
    # For now, just list what needs to be done
    echo "  - Would add since-count tracking"
done

echo ""
echo "Done listing scripts to update"
