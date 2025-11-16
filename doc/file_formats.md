# File Formats Documentation

This document describes the formats of the Elm package registry files and API responses used in the Elm compiler and tools.

## 1. registry.dat (Binary Format)

The `registry.dat` file is a binary cache of the package registry, stored locally in `$ELM_HOME/0.19.1/packages/registry.dat`. It contains the complete registry data in a compact binary format for fast loading.

### Structure

The file is serialized using Haskell's `Data.Binary` library with big-endian byte order. The top-level structure is:

```
Registry {
  count: Int64
  versions: Map<PackageName, KnownVersions>
}
```

Where:
- `count`: Total number of package versions across all packages (Int64, big-endian)
- `versions`: A map from package names to their known versions

### Map Serialization

Maps are serialized as:
- Number of entries: Word64 (big-endian)
- Followed by that many key-value pairs

### PackageName Serialization

Package names (e.g., "elm/core") are serialized as:
- Author length: Word8 (0-255)
- Author bytes: UTF-8 bytes
- Project length: Word8 (0-255)  
- Project bytes: UTF-8 bytes

### Version Serialization

Versions are serialized as:
- If major < 255 and minor < 256 and patch < 256:
  - Major: Word8
  - Minor: Word8  
  - Patch: Word8
- Otherwise:
  - Marker: Word8 = 255
  - Major: Word64 (big-endian)
  - Minor: Word64 (big-endian)
  - Patch: Word64 (big-endian)

### KnownVersions Serialization

For each package:
```
KnownVersions {
  newest: Version
  previous: List<Version>
}
```

Serialized as:
- `newest`: Version
- `previous`: List of versions in descending order (newest to oldest after the main newest)

Lists are serialized as:
- Length: Word64 (big-endian)
- Followed by that many elements

### Example

From the binary file `wrap/test/registry.dat`:

- Total count: 16446 versions
- Number of packages: 2946
- First package: "0ui/elm-task-parallel" with newest 2.0.0, previous versions appear to be stored (though the exact serialization for this entry appears anomalous in the sample file)
- Second package: "1602/elm-feather" with newest 2.3.5, previous 12 versions: 2.3.4, 2.3.3, ..., 1.0.0

Note: The binary format matches the Haskell Data.Binary serialization, but some entries in the sample file may have inconsistencies. The format is designed for efficient storage and lookup.

## 2. all-packages JSON Format

The `all-packages` endpoint at `https://package.elm-lang.org/all-packages` returns a JSON object containing all published packages and their versions.

### Structure

```json
{
  "author/project": ["1.0.0", "1.0.1", "2.0.0"],
  "elm/core": ["1.0.0", "1.0.1", "1.0.2", ...],
  ...
}
```

- Keys: Package names in "author/project" format (string)
- Values: Arrays of version strings in ascending order (oldest first)

### Version Format

Versions are semantic version strings: "major.minor.patch"

### Example

From `wrap/test/all-packages.json`:

```json
{
  "0ui/elm-task-parallel": [
    "1.0.0",
    "1.0.1", 
    "1.0.2",
    "2.0.0"
  ],
  "1602/elm-feather": [
    "1.0.0",
    "1.0.1",
    "1.0.2",
    "2.0.0",
    "2.0.1",
    "2.1.0",
    "2.2.0",
    "2.3.0",
    "2.3.1",
    "2.3.2",
    "2.3.3",
    "2.3.4",
    "2.3.5"
  ],
  ...
}
```

## 3. all-packages/since/{count} Response Format

The incremental update endpoint `https://package.elm-lang.org/all-packages/since/{count}` returns only the new packages published since the last known count.

### Structure

```json
[
  "author/project@1.2.3",
  "another/package@2.0.0",
  ...
]
```

- An array of strings
- Each string: "author/project@major.minor.patch"
- Ordered by publication time (newest packages first)

### Parameters

- `{count}`: The total number of package versions known locally (from registry.dat)

### Response Behavior

- If there are new packages since the given count, returns the array of new "name@version" strings
- If no new packages, returns an empty array `[]`

### Example

```json
[
  "robvandenbogaard/elm-compurob-explorations@1.1.4",
  "eigenwijskids/elm-playground-eigenwijs@13.0.0",
  "avarda-ab/avarda-ui@3.0.2",
  "canceraiddev/elm-form-builder@24.0.1",
  "maca/form-toolkit@1.0.5",
  "elm/html@1.0.1",
  "robvandenbogaard/elm-compurob-explorations@1.1.3",
  "elm/virtual-dom@1.0.5",
  "Warry/elm-stuff@1.0.0",
  "maca/form-toolkit@1.0.4",
  "vito/elm-ansi@12.0.0",
  "georgesboris/elm-widgets-alpha@4.15.1",
  "georgesboris/elm-widgets-alpha@4.15.0",
  "georgesboris/elm-widgets-alpha@4.14.0",
  "the-sett/elm-mlir@1.0.0",
  "maca/form-toolkit@1.0.3",
  "georgesboris/elm-widgets-alpha@4.13.0",
  "coreygirard/elm-zipper@1.0.0",
  "scrive/elm-personal-number@1.1.0",
  "avarda-ab/avarda-ui@3.0.1",
  "brainrake/elm-unsafe@1.0.1",
  "brainrake/elm-unsafe@1.0.0",
  "georgesboris/elm-widgets-alpha@4.12.0",
  "georgesboris/elm-widgets-alpha@4.11.2",
  "terezka/elm-charts@5.0.0",
  "maca/form-toolkit@1.0.2",
  "maca/form-toolkit@1.0.1",
  "maca/form-toolkit@1.0.0",
  "georgesboris/elm-widgets-alpha@4.11.1",
  "georgesboris/elm-widgets-alpha@4.11.0",
  "NoRedInk/noredink-ui@35.6.0",
  "georgesboris/elm-widgets-alpha@4.10.2",
  "jxxcarlson/scripta-compiler-v2@1.3.0",
  "georgesboris/elm-widgets-alpha@4.10.1",
  "avarda-ab/avarda-ui@3.0.0",
  "avarda-ab/avarda-ui@2.5.0",
  "georgesboris/elm-widgets-alpha@4.10.0",
  "eigenwijskids/elm-playground-eigenwijs@12.0.0",
  "avarda-ab/avarda-ui@2.4.0",
  "robvandenbogaard/webgl-playground@1.0.0",
  "avarda-ab/avarda-ui@2.3.0",
  "georgesboris/elm-widgets-alpha@4.9.0",
  "rl-king/elm-gallery@2.1.0",
  "vito/elm-ansi@11.0.1",
  "georgesboris/elm-widgets-alpha@4.8.1",
  "enkidatron/elm-cldr@3.1.0"
]
```

This example shows the response for `all-packages/since/16400`, containing 46 new package versions published after the given count.</content>
<parameter name="filePath">/Volumes/Devel/Projects/Elm-Compiler/wrap/doc/file_formats.md