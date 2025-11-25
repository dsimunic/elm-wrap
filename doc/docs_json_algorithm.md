# Docs.json Generation Algorithm Analysis

## Overview

The Elm compiler generates `docs.json` files for packages to provide structured documentation data. This document analyzes the algorithm used in the Haskell-based Elm compiler and provides recommendations for implementing an equivalent C version.

## Current Haskell Implementation

### Key Components

The documentation generation is handled in `compiler/src/Elm/Docs.hs`. The process involves:

1. **Parsing**: Elm source code is parsed into an AST (`AST.Source`)
2. **Canonicalization**: The AST is transformed into canonical form (`AST.Canonical`)
3. **Documentation Extraction**: From the canonical module, documentation data is extracted

### Data Structures

The `docs.json` contains a list of modules with the following structure:

```json
[
  {
    "name": "Module.Name",
    "comment": "Module documentation...",
    "unions": [...],
    "aliases": [...],
    "values": [...],
    "binops": [...]
  }
]
```

Each section contains documented items with:
- `name`: The identifier name
- `comment`: Associated documentation comment
- `type`: Type signature (for values and binops)
- Additional fields specific to unions/aliases (args, cases, etc.)

### Algorithm Steps

1. **Extract Module Overview**
   - Parse the module's documentation comment
   - Look for `@docs` directives to identify documented items
   - Validate that documented names are properly exported

2. **Process Exports**
   - For each exported item, retrieve its definition
   - Extract associated documentation comment
   - Convert type information to JSON-serializable format

3. **Type Extraction**
   - Values and binops: Use annotated type signatures
   - Unions: Extract constructor information and type variables
   - Aliases: Extract type alias definitions

4. **Validation**
   - Ensure all `@docs` items are exported
   - Verify type annotations exist for documented values
   - Check for duplicate documentation entries

## What Gets Extracted from Source

### Module Level
- Module name (from `module Module.Name exposing (..)`)
- Module documentation comment (before module declaration)
- Export list

### Per Item
- **Values/Functions**: Name, documentation comment, type annotation
- **Type Unions**: Name, comment, type variables, constructor definitions
- **Type Aliases**: Name, comment, type variables, aliased type
- **Binary Operators**: Name, comment, type, associativity, precedence

### Comments
- Documentation comments are associated by proximity to definitions
- The overview comment contains `@docs` directives listing documented items

## Parsing Requirements for C Implementation

### Complexity Analysis

Elm's syntax includes:
- Significant whitespace (indentation-based)
- Complex type syntax (records, unions, functions, generics)
- Nested expressions and declarations
- Special syntax for operators, imports, etc.

### String Manipulation vs. Proper Parsing

**String Manipulation Limitations:**
- Cannot reliably handle nested structures
- Fails with complex type signatures
- Brittle to syntax changes
- Cannot validate syntax correctness
- Difficult to extract structured type information

**Proper Parsing Advantages:**
- Handles all Elm syntax correctly
- Provides structured AST for reliable extraction
- Enables validation and error reporting
- Future-proof against language changes

### Recommendation: Use Tree-Sitter Elm Parser

**Rationale:**
- Tree-sitter provides a robust, battle-tested parser for Elm
- Generates a structured syntax tree that's easy to traverse
- Handles all edge cases and complex syntax
- Available as a C library
- Used by many editors and tools for Elm

**Implementation Approach:**
1. Use tree-sitter-elm to parse source files
2. Traverse the AST to extract:
   - Module declarations
   - Export lists
   - Documentation comments
   - Type annotations
   - Union and alias definitions
3. Build the documentation structure
4. Serialize to JSON

### Alternative: Port Haskell Parser

Porting the Haskell parsing logic to C would be extremely complex and error-prone. Tree-sitter is the recommended approach for reliability and maintainability.

## Implementation Plan

1. **Setup Tree-Sitter Integration**
   - Include tree-sitter-elm as a dependency
   - Create C bindings if necessary

2. **AST Traversal**
   - Implement visitors for module, declarations, comments
   - Extract structured data from parse tree

3. **Documentation Logic**
   - Replicate the validation and extraction logic from `Docs.hs`
   - Handle `@docs` parsing
   - Convert types to JSON format

4. **Testing**
   - Compare output with Haskell compiler's `docs.json`
   - Test with various Elm packages
   - Validate against edge cases

## Conclusion

Creating a C version requires proper parsing capabilities. Tree-sitter provides the most reliable and maintainable solution, avoiding the pitfalls of string manipulation for a complex language like Elm. The algorithm itself is straightforward once the structured data is available from the parser.