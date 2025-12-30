#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

# Add since-count tracking to all test functions in the test scripts

SCRIPTS_DIR="/Volumes/Devel/Projects/elm-wrap/test/scripts/install-uninstall"

add_since_tracking() {
    local file="$1"
    local temp_file="${file}.tmp"
    
    echo "Processing $(basename "$file")..."
    
    # Use sed to add since-count tracking
    # This is a simpler approach - just wrap each test function
    sed -E '
        # After a function declaration line followed by echo BEGIN, insert tracking
        /^test_[a-zA-Z0-9_]+\(\) \{$/ {
            N
            s/^(test_[a-zA-Z0-9_]+\(\) \{)\n(    echo "=== BEGIN)/\1\n    local since_before since_after delta\n    since_before=$(get_v1_since_count)\n    \n\2/
        }
        
        # Before echo END, insert delta output
        /^    echo "=== END/ {
            i\
    since_after=$(get_v1_since_count)\
    delta=$((since_after - since_before))\
    echo "[Registry since-count: $since_before -> $since_after (Δ$delta)]"
        }
    ' "$file" > "$temp_file"
    
    mv "$temp_file" "$file"
    echo "  ✓ Updated $(basename "$file")"
}

# Process all test-* scripts (without .sh extension)
for script in "$SCRIPTS_DIR"/test-*; do
    # Skip if not executable or not a regular file
    [[ -f "$script" && -x "$script" ]] || continue
    add_since_tracking "$script"
done

# Process all test scripts
for script in test/scripts/install-uninstall/test-*; do
    if [ -f "$script" ] && [ -x "$script" ]; then
        add_since_tracking "$script"
    fi
done

echo ""
echo "All test scripts updated!"
echo "Run: cd test/scripts/install-uninstall && ./run-tests.sh to regenerate expected files"
