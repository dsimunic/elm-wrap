# Built-in Rules

elm-wrap ships with pre-compiled policy rules embedded directly in the binary. These rules can be used without needing external rule files.

## Using Built-in Rules

Built-in rules can be referenced by name (without path separators):

```bash
# List all available built-in rules
elm-wrap policy built-in

# View a built-in rule's source
elm-wrap policy view no_unused_dependencies

# Use a built-in rule for package review
elm-wrap review package . --rule no_unused_dependencies
```

When you specify a rule by name only (no `/` or `\` in the name), elm-wrap:
1. First checks the embedded built-in rules
2. Falls back to the file system if not found

To explicitly use a file system rule instead of a built-in, include a path:
```bash
elm-wrap policy view ./my_rules/no_unused_dependencies
```

## Available Built-in Rules

| Rule | Description |
|------|-------------|
| `core_package_files` | Identifies core files required in an Elm package |
| `no_invalid_package_layout` | Validates package directory structure |
| `no_missing_type_expose` | Checks that types used in exposed APIs are also exposed |
| `no_redundant_elm_files` | Detects Elm files not reachable from exposed modules |
| `no_redundant_files` | Detects files that shouldn't be in a published package |
| `no_unused_dependencies` | Finds dependencies declared but not imported |
| `publish_files` | Identifies files that will be included when publishing |

## Technical Implementation

### Embedding Mechanism

Built-in rules are embedded using a zip archive appended to the executable binary. This technique (sometimes called a "self-extracting" or "polyglot" binary) works because:

1. **Executable format tolerance**: Operating systems read executable metadata from specific locations (headers at the start for ELF/Mach-O, or from PE headers on Windows). Extra data appended to the end is ignored by the loader.

2. **ZIP format design**: ZIP files store their central directory at the *end* of the file, with offsets pointing backward to the file data. This means a ZIP reader can find valid ZIP data even when prepended with arbitrary bytes.

### Build Process

During `make all`:

1. **Compile rules**: The `rulrc` compiler processes all `.dl` files in `rulr/rules/`:
   ```
   rulr/rules/*.dl → build/builtin_rules/*.dlc
   ```

2. **Create archive**: Compiled rules are zipped:
   ```
   build/builtin_rules/*.dlc → build/builtin_rules.zip
   ```

3. **Append to binary**: The zip is concatenated to the executable:
   ```
   cat build/builtin_rules.zip >> bin/elm-wrap
   ```

### Runtime Loading

At startup, elm-wrap:

1. Determines its own executable path using platform-specific APIs:
   - macOS: `_NSGetExecutablePath()`
   - Linux: `readlink("/proc/self/exe")`

2. Searches backward from the end of the file for the ZIP end-of-central-directory record

3. Calculates the offset where the ZIP archive begins

4. Opens the ZIP using miniz with the correct offset and size

5. Caches the list of available rule names for quick lookup

### File Structure

```
bin/elm-wrap
├── [Mach-O/ELF executable data]
└── [ZIP archive]
    ├── core_package_files.dlc
    ├── no_invalid_package_layout.dlc
    ├── no_missing_type_expose.dlc
    ├── no_redundant_elm_files.dlc
    ├── no_redundant_files.dlc
    ├── no_unused_dependencies.dlc
    └── publish_files.dlc
```

### Source Files

| File | Purpose |
|------|---------|
| `src/rulr/builtin_rules.h` | Public API for accessing built-in rules |
| `src/rulr/builtin_rules.c` | Implementation using miniz for ZIP reading |
| `src/rulr/rulr_dl.c` | Rule loading with built-in fallback logic |
| `src/commands/policy/policy.c` | `policy built-in` and `policy view` commands |

### Adding New Built-in Rules

To add a new built-in rule:

1. Create the rule file in `rulr/rules/`:
   ```
   rulr/rules/my_new_rule.dl
   ```

2. Rebuild:
   ```bash
   make clean all
   ```

3. Verify it's included:
   ```bash
   ./bin/elm-wrap policy built-in
   ```

The rule will automatically be compiled, zipped, and embedded in the binary.

## Debugging

### Verify embedded rules exist
```bash
# Using system unzip (will show warning about extra bytes)
unzip -t bin/elm-wrap

# Using elm-wrap itself
./bin/elm-wrap policy built-in
```

### Check binary size
```bash
# Original binary size (before zip append)
ls -la bin/elm-wrap  # during build, before append

# Final size includes zip
ls -la bin/elm-wrap  # after build complete
ls -la build/builtin_rules.zip  # size of embedded rules
```

### Extract embedded rules
```bash
# Extract to inspect (creates files in current directory)
unzip bin/elm-wrap -d /tmp/extracted-rules/
```
