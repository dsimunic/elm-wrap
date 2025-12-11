# indexmaker

A utility for generating `registry.dat` files compatible with the Elm compiler and **elm-wrap**.

## Overview

`indexmaker` creates binary registry files from simple text-based package specifications. This is useful for:
- Testing registry-related functionality
- Creating minimal test environments
- Generating custom package registries

## Building

```bash
make indexmaker
```

The compiled binary will be placed in `bin/indexmaker`.

## Usage

```bash
indexmaker <input-file> <output-file>
indexmaker - <output-file>           # Read from stdin
```

### Input Format

One package per line in the format `author/package@version`:

```
elm/core@1.0.5
elm/html@1.0.0
elm/json@1.1.3
```

**Features:**
- Comments: Lines starting with `#` are ignored
- Empty lines are ignored
- Multiple versions of the same package are supported
- Packages are automatically sorted alphabetically in the output

### Example Input File

```
# Core Elm packages
elm/core@1.0.0
elm/core@1.0.5

# HTML support
elm/html@1.0.0
elm/html@1.0.1

# JSON handling
elm/json@1.0.0
elm/json@1.1.3
```

## Examples

### From a file

```bash
indexmaker packages.txt registry.dat
```

### From stdin

```bash
cat packages.txt | indexmaker - registry.dat
```

```bash
printf "elm/core@1.0.5\nelm/html@1.0.0\n" | indexmaker - registry.dat
```

### Creating a test ELM_HOME

To create a registry that `wrap` can use:

```bash
# Create directory structure
mkdir -p /tmp/test_elm_home/0.19.1/packages

# Generate registry
printf "elm/core@1.0.5\nelm/html@1.0.0\n" | indexmaker - /tmp/test_elm_home/0.19.1/packages/registry.dat

# Test with wrap
env ELM_HOME=/tmp/test_elm_home wrap debug registry_v1 list
```

### Piping from curl

You can create a registry directly from the `/since` endpoint format:

```bash
# The /since endpoint returns: ["author/package@version", ...]
# Strip JSON array syntax and feed to indexmaker
curl -s "https://package.elm-lang.org/all-packages/since/16400" \
  | jq -r '.[]' \
  | indexmaker - registry.dat
```

## Output Format

The output is a binary `registry.dat` file in the format used by:
- The official Elm compiler (`elm`)
- **elm-wrap** (`wrap`)

The file structure follows the Haskell `Data.Binary` serialization format with big-endian byte order. See [`../doc/file_formats.md`](../doc/file_formats.md) for detailed format documentation.

## Error Handling

The tool will:
- Warn about invalid package specifications but continue processing
- Exit with an error if no valid packages are found
- Exit with an error if the output file cannot be written

## Testing

Run the smoke test to verify the tool works correctly:

```bash
./smoke
```

## Implementation

The tool uses the same registry code as **elm-wrap** itself:
- `src/registry.c` - Registry data structures and serialization
- `src/commands/package/package_common.c` - Package name and version parsing
- `src/alloc.c` - Arena memory allocator

This ensures 100% compatibility with the main `wrap` binary.
