# Test Registry Tools

Utilities for creating and populating test registries for **elm-wrap** development and testing.

## indexmaker

A utility for generating `registry.dat` files compatible with the Elm compiler and **elm-wrap**.

### Overview

`indexmaker` creates binary registry files from simple text-based package specifications. This is useful for:
- Testing registry-related functionality
- Creating minimal test environments
- Generating custom package registries

### Building

```bash
make indexmaker
```

The compiled binary will be placed in `bin/tools/indexmaker`.

### Usage

```bash
indexmaker <input-file> <output-file>
indexmaker - <output-file>           # Read from stdin
```

#### Input Format

`indexmaker` auto-detects the input format by reading the first line:
- **V2 registry text format**: first line is `format 2`
- **V1 package list**: otherwise, first line must be `author/package@version`

For V1 package lists, comments/empty lines are supported after the first line.

##### V1 Package List

One package per line in the format `author/package@version`:

```
elm/core@1.0.5
elm/html@1.0.0
elm/json@1.1.3
```

**Features:**
- Comments: Lines starting with `#` are ignored (after the first line)
- Empty lines are ignored (after the first line)
- Multiple versions of the same package are supported
- Packages are automatically sorted alphabetically in the output

#### Example Input File

```
elm/core@1.0.0
elm/core@1.0.5

# HTML support
elm/html@1.0.0
elm/html@1.0.1

# JSON handling
elm/json@1.0.0
elm/json@1.1.3
```

##### V2 Registry Text Format

Provide a V2 registry text file starting with `format 2` (for example `test/data/imaginary-package-registry.txt`).

### Examples

#### From a file

```bash
indexmaker packages.txt registry.dat
```

#### From stdin

```bash
cat packages.txt | indexmaker - registry.dat
```

```bash
printf "elm/core@1.0.5\nelm/html@1.0.0\n" | indexmaker - registry.dat
```

#### Creating a test ELM_HOME

To create a registry that `wrap` can use:

```bash
# Create directory structure
mkdir -p /tmp/test_elm_home/0.19.1/packages

# Generate registry
printf "elm/core@1.0.5\nelm/html@1.0.0\n" | indexmaker - /tmp/test_elm_home/0.19.1/packages/registry.dat

# Test with wrap
env ELM_HOME=/tmp/test_elm_home wrap debug registry_v1 list
```

#### Piping from curl

You can create a registry directly from the `/since` endpoint format:

```bash
# The /since endpoint returns: ["author/package@version", ...]
# Strip JSON array syntax and feed to indexmaker
curl -s "https://package.elm-lang.org/all-packages/since/16400" \
  | jq -r '.[]' \
  | indexmaker - registry.dat
```

### Output Format

The output is a binary `registry.dat` file in the format used by:
- The official Elm compiler (`elm`)
- **elm-wrap** (`wrap`)

The file structure follows the Haskell `Data.Binary` serialization format with big-endian byte order. See [`../doc/file_formats.md`](../doc/file_formats.md) for detailed format documentation.

### Error Handling

The tool will:
- Warn about invalid package specifications but continue processing
- Exit with an error if no valid packages are found
- Exit with an error if the output file cannot be written

### Testing

Run the smoke test to verify the tool works correctly:

```bash
./smoke
```

### Implementation

The tool uses the same registry code as **elm-wrap** itself:
- `src/registry.c` - Registry data structures and serialization
- `src/commands/package/package_common.c` - Package name and version parsing
- `src/alloc.c` - Arena memory allocator

This ensures 100% compatibility with the main `wrap` binary.

## mkpkg

A utility for populating package directories from V2 registry files. Creates complete package structures with `elm.json` files and empty `src/` directories.

### Overview

`mkpkg` reads a V2 registry file (like those used by **elm-wrap**'s solver) and creates package directories in the ELM_HOME cache structure. This is useful for:
- Populating test environments with package metadata
- Creating minimal package structures for testing
- Setting up offline development environments

### Building

```bash
make mkpkg
```

The compiled binary will be placed in `bin/mkpkg`.

### Usage

```bash
mkpkg REGISTRY PACKAGE
```

**Arguments:**
- `REGISTRY`: Path to V2 registry file (e.g., `test/data/imaginary-package-registry.txt`)
- `PACKAGE`: Package specification in `author/name` format

**Environment:**
- `ELM_HOME`: Target directory (optional, defaults to `~/.elm/VERSION`)
- `ELM_VERSION`: Elm version subdirectory (optional, defaults to compiler version or `0.19.1`)

### Input Format

The tool reads V2 registry format files with the structure:

```
format 2
elm 0.19.1

package: elm/core
    version: 1.0.0
    status: valid
    license: BSD-3-Clause
    dependencies:

package: elm/json
    version: 1.0.0
    status: valid
    license: BSD-3-Clause
    dependencies:
        elm/core  1.0.0 <= v < 2.0.0
```

### Examples

#### Create a single package

```bash
# Create elm/core in test environment
export ELM_HOME=/tmp/test-elm
mkpkg test/data/imaginary-package-registry.txt elm/core
```

This creates:
```
/tmp/test-elm/0.19.1/packages/elm/core/1.0.0/
├── elm.json
└── src/
```

#### Create multiple packages

```bash
# Set up a test environment with several packages
export ELM_HOME=/tmp/test-elm
mkpkg test/data/imaginary-package-registry.txt elm/core
mkpkg test/data/imaginary-package-registry.txt elm/json
mkpkg test/data/imaginary-package-registry.txt elm/http
```

#### Use with default ELM_HOME

```bash
# Creates packages in ~/.elm/0.19.1/packages/
mkpkg test/data/imaginary-package-registry.txt elm/core
```

### Output Structure

For each version of a package, `mkpkg` creates:

```
$ELM_HOME/$VERSION/packages/author/name/version/
├── elm.json          # Package metadata with dependencies
└── src/              # Empty source directory
```

The `elm.json` file contains:
- Package type, name, version
- Compiler version range (e.g., `"0.19.0 <= v < 0.20.0"`)
- Dependencies from the registry (as version constraints)
- Empty exposed-modules list
- Empty test-dependencies

**Example generated `elm.json`:**

```json
{
    "type": "package",
    "name": "elm/json",
    "summary": "generated by mkpkg",
    "license": "BSD-3-Clause",
    "version": "1.0.0",
    "exposed-modules": [],
    "elm-version": "0.19.0 <= v < 0.20.0",
    "dependencies": {
        "elm/core": "1.0.0 <= v < 2.0.0"
    },
    "test-dependencies": {}
}
```

### Error Handling

The tool will:
- Exit with an error if the package is not found in the registry
- Warn and continue if a directory cannot be created
- Warn and continue if an `elm.json` file cannot be written
- Use default paths if `ELM_HOME` is not set

### Implementation

The tool uses the same code as **elm-wrap** itself:
- `src/protocol_v2/solver/v2_registry.c` - V2 registry parsing
- `src/cache.c` - Cache path construction
- `src/elm_json.c` - elm.json data structures
- `src/alloc.c` - Arena memory allocator

This ensures the generated packages are compatible with both `elm` and `wrap`.
