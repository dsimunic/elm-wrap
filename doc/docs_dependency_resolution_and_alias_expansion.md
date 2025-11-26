# Documentation Generation: Dependency Resolution and Alias Expansion

## Overview

When generating Elm package documentation, `elm-wrap publish docs` resolves dependency types and expands module/type aliases and fully qualifies type signatures. This is consistent with Elm 0.19.1 compiler's output.

For example:

```elm
-- Source code (uses unqualified imports)
import Json.Decode exposing (..)
decode : Decoder a -> String -> Result Error a

-- Generated documentation (fully qualified)
decode : Json.Decode.Decoder a -> String.String -> Result.Result Json.Decode.Error a
```

This requires:
1. **Dependency Resolution**: Determining which types come from which modules
2. **Module Alias Expansion**: Expanding aliases like `D.Decoder` to `Json.Decode.Decoder`
3. **Type Alias Expansion**: Expanding type aliases when implementations have more parameters than type signatures suggest

---

## Part 1: Dependency Type Resolution

### Problem Statement

Modules often use wildcard imports:

```elm
import Json.Decode exposing (..)
```

This requires discovering the imported module's exports.

### Solution Architecture

The implementation uses a three-phase approach with lazy loading:

#### Phase 1: Cache Initialization

When `publish docs` starts:

1. Read the package's `elm.json` to discover dependencies
2. Get `ELM_HOME` path from cache configuration
3. Create a `DependencyCache` structure (initially empty)

#### Phase 2: Import Processing (Per Module)

When encountering `import ModuleName exposing (..)`:

1. Check if module exports are cached
2. If not cached:
   - Locate the module file in `$ELM_HOME/packages/{author}/{name}/{version}/src/`
   - Parse the module header
   - Extract the `exposing_list` to get all exported types
   - Cache the results
3. Add all exported types to the `ImportMap`

#### Phase 3: Type Qualification

When qualifying types in documentation:

1. Check if type is in `ImportMap` (now includes types from `exposing (..)`)
2. Check if type is defined locally in the module
3. Apply module alias expansion rules (see Part 2)

### Data Structures

#### DependencyCache

```c
struct DependencyCache {
    CachedModuleExports *modules;
    int modules_count;
    int modules_capacity;
    char *elm_home;              /* Path to ELM_HOME directory */
    char *package_path;          /* Path to package being documented */
};
```

#### CachedModuleExports

```c
struct CachedModuleExports {
    char *module_name;           /* e.g., "Json.Decode" */
    char **exported_types;       /* e.g., ["Decoder", "Value", "Error"] */
    int exported_types_count;
    bool parsed;                 /* false if lookup failed */
};
```

### Key Functions

| Function | Purpose |
|----------|---------|
| `dependency_cache_create()` | Initialize cache with ELM_HOME and package path |
| `dependency_cache_get_exports()` | Get or parse module exports (lazy loading) |
| `dependency_cache_find()` | Check if module is already cached |
| `find_module_in_dependencies()` | Locate module file in ELM_HOME |
| `parse_module_exports()` | Parse module header to extract exports |

### Module Export Parsing

The `parse_module_exports()` function creates the AST:

```
file_tree
  └── module_declaration
      ├── upper_case_qid (module name)
      └── exposing_list
          ├── exposed_type
          │   └── upper_case_identifier ("Decoder")
          ├── exposed_type
          │   └── upper_case_identifier ("Value")
          └── ...
```

When a module uses `exposing (..)`, the function scans for all `type_declaration` and `type_alias_declaration` nodes.

### Implicit Imports

Elm implicitly imports several modules from `elm/core`. The `apply_implicit_imports()` function sets up:

```elm
import Basics exposing (..)
import List exposing (List, (::))
import Maybe exposing (Maybe(..))
import Result exposing (Result(..))
import String exposing (String)
import Char exposing (Char)
import Tuple
import Debug
import Platform exposing (Program)
import Platform.Cmd as Cmd exposing (Cmd)
import Platform.Sub as Sub exposing (Sub)
```

These are tracked as direct imports with appropriate aliases.

### Performance Characteristics

- **Startup**: O(1) - only allocates empty cache structure
- **First Access**: O(n) where n = file size (parses module header once)
- **Subsequent Access**: O(1) - returns cached result
- **Memory**: O(m × t) where m = modules accessed, t = avg types per module

### Error Handling

The system never fails documentation generation:

| Scenario | Behavior |
|----------|----------|
| Missing module file | Add cache entry with `parsed = false`, continue |
| Parse failure | Return empty exports, use fallback behavior |
| Missing ELM_HOME | Cache not created, falls back to hardcoded types |

All errors go to stderr, keeping stdout clean for JSON output.

---

## Part 2: Module Alias Expansion

### Module Import Types

Elm supports two types of module imports relevant to alias expansion:

1. **Direct imports**: Makes a module available by its name
   ```elm
   import Html
   import Json.Decode
   ```

2. **Aliased imports**: Makes a module available **only** under the alias name
   ```elm
   import Json.Schema.Validation as Validation
   import Html.Attributes as Html
   ```

**Important**: When a module is imported with an alias, the original module name is **no longer available**. The alias completely replaces the original name in that scope.

```elm
import Html.Attributes as Html

-- This works:
val : Html.Attribute msg

-- This FAILS with "I cannot find a `Html.Attributes` import":
val : Html.Attributes.Attribute msg
```

**Ambiguous aliases**: When two **different** modules are imported with the same alias, any usage of that alias is ambiguous and will cause an error:

```elm
import Array.Extra as Array
import ArrayExtra as Array      -- DIFFERENT module, same alias

val = Array.interweave [1,2] [3,4]  -- ERROR: Ambiguous name
```

**Same module, different forms**: If the **same** module is imported multiple times (e.g., with and without alias), this is NOT ambiguous - both refer to the same module:

```elm
import Html.Attributes as Html
import Html.Attributes        -- Same module, just adding direct access

n = Html.Attributes.name       -- PASS (direct import works)
n = Html.name                  -- PASS (alias still works for same module)
```

### Data Structures

#### ImportMap
Tracks types explicitly exposed from imported modules:
```c
typedef struct {
    char *type_name;       /* e.g., "Schema" */
    char *module_name;     /* e.g., "Json.Schema.Definitions" */
} TypeImport;
```

#### ModuleAliasMap
Tracks module aliases with ambiguity detection:
```c
typedef struct {
    char *alias;           /* e.g., "Validation" */
    char *full_module;     /* e.g., "Json.Schema.Validation" */
    bool is_ambiguous;     /* True if multiple different modules use this alias */
    char *ambiguous_with;  /* If ambiguous, the other module name */
} ModuleAlias;
```

#### DirectModuleImports
Tracks modules that are directly imported (without an alias):
```c
typedef struct {
    char **modules;        /* e.g., ["Html", "Json.Decode", "String"] */
    int modules_count;
} DirectModuleImports;
```

**Note**: Aliased imports are NOT added to this list. When `import Foo as F` is used, `Foo` is unavailable and only `F` works in Elm code.

### Alias Expansion Algorithm

When processing a qualified type reference like `Foo.Bar`:

1. **Check if already qualified**: If preceded by a dot (e.g., `Module.Foo.Bar`), keep as-is

2. **Check if it's a module prefix**: If followed by a dot (e.g., `Foo.Bar`):
   - Look up if `Foo` is a module alias
   - Check if the alias is ambiguous (refers to multiple different modules)
   - If ambiguous, report a warning and keep as-is
   - If it's a valid alias, expand to the full module name
   - If not an alias, check if it matches a directly imported module

3. **Check if it's an unqualified type**: If standalone (e.g., `Foo`):
   - Check if it's a local type (defined in current module)
   - Check if it's in the ImportMap
   - Keep as-is if not found (likely a type variable)

### Expansion Rule (Code)

```c
bool is_ambiguous = false;
const char *ambig_mod1, *ambig_mod2;
const char *full_module = lookup_module_alias(alias_map, "Prefix", &is_ambiguous, &ambig_mod1, &ambig_mod2);
if (is_ambiguous) {
    // AMBIGUOUS: Two different modules use the same alias
    // Report warning and keep unexpanded
    fprintf(stderr, "Warning: Ambiguous alias '%s' - refers to both '%s' and '%s'\n", ...);
} else if (full_module != NULL) {
    // Alias found - expand to full module name
}
```

### Examples

#### Example 1: Alias Expansion (No Ambiguity)

```elm
import Json.Schema.Validation as Validation

validate : Validation.ValidationOptions -> ...
```

- `Validation` is an alias → returns `"Json.Schema.Validation"`
- `Validation` is NOT directly imported → `false`
- `should_expand = true` → **Expand**

**Result**: `Json.Schema.Validation.ValidationOptions`

#### Example 2: Ambiguous Alias (Error)

```elm
import Array.Extra as Array
import ArrayExtra as Array        -- DIFFERENT module, same alias

interweave : Array.InterweaveResult -> ...
```

- `Array` is an alias that maps to BOTH `Array.Extra` AND `ArrayExtra`
- `is_ambiguous = true` → **Report warning, keep unexpanded**

**Result**: Warning printed to stderr, `Array.InterweaveResult` kept as-is

#### Example 3: Dual Import (Both Direct and Aliased)

```elm
import Html                    -- Direct import of Html module
import Html.Attributes         -- Direct import (no alias)
import Json.Decode as D        -- Aliased import

dropdown : List (Html.Attributes.Attribute msg) -> Html.Html msg
decode : D.Decoder a -> String -> Result D.Error a
```

For `Html.Attributes.Attribute`:
- `Html.Attributes` is directly imported → no alias lookup needed
- Keep as-is (already fully qualified)

For `D.Decoder`:
- `D` is an alias → returns `"Json.Decode"`
- Expand to `Json.Decode.Decoder`

**Result**: `Html.Attributes.Attribute msg`, `Json.Decode.Decoder a`

### Import Processing

When processing import statements, the algorithm implements **"last import wins"** semantics:

1. **Direct imports** (no `as` clause): 
   - Remove any existing alias for this module
   - Add module name to direct imports list

2. **Aliased imports** (with `as` clause): 
   - Remove module from direct imports (if present)
   - Add/update mapping from alias → full module name

```c
if (has_as_clause) {
    // Aliased import: original name no longer available
    remove_direct_import(direct_imports, module_name);
    add_module_alias(alias_map, alias_name, module_name);
} else {
    // Direct import: module available by its name, alias removed
    remove_alias_for_module(alias_map, module_name);
    add_direct_import(direct_imports, module_name);
}
```

### Rationale

The algorithm matches Elm compiler behavior: **when a module is aliased, only the alias is valid in source code**. Documentation generation must expand aliases to produce fully qualified type names that reference the actual module structure.

---

## Part 3: Type Alias Expansion

### Problem

Given:
```elm
type alias Decoder a =
    Context -> Edn -> Result String a

andThen : (a -> Decoder b) -> Decoder a -> Decoder b
andThen fn decoder ctx edn =
    case decoder ctx edn of
        Ok value -> fn value ctx edn
        Err err -> Err err
```

The generated docs may show:
```json
"type": "(a -> Decoder b) -> Decoder a -> Context -> Edn -> Result String b"
```

Instead of:
```json
"type": "(a -> Decoder b) -> Decoder a -> Decoder b"
```

### Root Cause

This is not a bug but a natural consequence of how the Elm compiler canonicalizes typed function definitions.

### The Algorithm

#### Phase 1: Gathering Typed Arguments

When canonicalizing a definition with a type annotation, the compiler matches source-level arguments against the declared type:

```
gatherTypedArgs(env, name, sourceArgs, currentType, index, accumulator):
    if sourceArgs is empty:
        return (reverse(accumulator), currentType)  -- currentType becomes resultType
    
    srcArg = head(sourceArgs)
    remainingArgs = tail(sourceArgs)
    
    -- KEY STEP: Dealias to find function arrows
    dealasedType = iteratedDealias(currentType)
    
    if dealasedType is TLambda(argType, resultType):
        canonicalizedArg = canonicalize(env, srcArg)
        newAccumulator = (canonicalizedArg, argType) : accumulator
        return gatherTypedArgs(env, name, remainingArgs, resultType, index+1, newAccumulator)
    else:
        error("Annotation too short for number of arguments")
```

The `iteratedDealias` function recursively unwraps type aliases:

```
iteratedDealias(type):
    if type is TAlias(home, name, args, realType):
        expandedType = dealias(args, realType)
        return iteratedDealias(expandedType)
    else:
        return type
```

#### Phase 2: Storing the Canonical Definition

The canonical AST stores:
```
TypedDef(name, freeVars, typedArgs, body, resultType)
```

Where:
- `typedArgs` = list of (pattern, type) pairs for each source argument
- `resultType` = remaining type after consuming all source arguments

#### Phase 3: Reconstructing Type for Documentation

```
reconstructType(typedArgs, resultType):
    return foldr(TLambda, resultType, map(snd, typedArgs))
```

This rebuilds `arg1 -> arg2 -> ... -> resultType`.

### Worked Example

For `andThen fn decoder ctx edn`:

1. **Initial type:** `(a -> Decoder b) -> Decoder a -> Decoder b`

2. **Process `fn`:**
   - Current: `(a -> Decoder b) -> Decoder a -> Decoder b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `(a -> Decoder b)`, remaining = `Decoder a -> Decoder b`

3. **Process `decoder`:**
   - Current: `Decoder a -> Decoder b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `Decoder a`, remaining = `Decoder b`

4. **Process `ctx`:**
   - Current: `Decoder b` (alias for `Context -> Edn -> Result String b`)
   - `iteratedDealias` → `Context -> Edn -> Result String b` (a `TLambda`!)
   - Consume: argType = `Context`, remaining = `Edn -> Result String b`

5. **Process `edn`:**
   - Current: `Edn -> Result String b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `Edn`, remaining = `Result String b`

6. **Result:**
   - `typedArgs` = [(fn, `a -> Decoder b`), (decoder, `Decoder a`), (ctx, `Context`), (edn, `Edn`)]
   - `resultType` = `Result String b`

7. **Reconstruction:**
   - `(a -> Decoder b) -> Decoder a -> Context -> Edn -> Result String b`

### Implementation in elm-wrap

The `expand_function_type_aliases()` function in `elm_docs.c` replicates this behavior:

1. Count implementation parameters from the source
2. Count type arrows in the annotation
3. If implementation has more params, expand the return type alias
4. Substitute type variables with actual type arguments

### When This Matters

Type alias expansion occurs when:
- A function implementation has more parameters than its type signature suggests
- The return type is a type alias that is itself a function type

---

## Implementation Files

| File | Purpose |
|------|---------|
| `src/commands/publish/docs/dependency_cache.h` | Cache API and data structures |
| `src/commands/publish/docs/dependency_cache.c` | Cache implementation (~360 lines) |
| `src/commands/publish/docs/elm_docs.c` | Main documentation parser |
| `src/commands/publish/docs/docs.c` | Entry point, cache initialization |

### Key Functions in elm_docs.c

| Function | Purpose |
|----------|---------|
| `extract_imports()` | Parse imports, populate ImportMap and aliases |
| `apply_implicit_imports()` | Set up Elm's implicit imports |
| `qualify_type_names()` | Fully qualify all types in a signature |
| `expand_function_type_aliases()` | Expand type aliases when needed |
| `extract_type_expression()` | Extract and canonicalize type expressions |

---

## Verification

### Build Verification
```bash
make clean all
```

### Test Command
```bash
bin/elm-wrap publish docs /path/to/package | jq . > output.json
```

### Grep Verification (No Direct Allocations)
```bash
grep -r --include="*.c" '\bmalloc\s*(' src/ | grep -v arena_malloc
grep -r --include="*.c" '\bcalloc\s*(' src/ | grep -v arena_calloc
grep -r --include="*.c" '\brealloc\s*(' src/ | grep -v arena_realloc
grep -r --include="*.c" '\bstrdup\s*(' src/ | grep -v arena_strdup
```

---

## Summary

The documentation generation system handles three related concerns:

1. **Dependency Resolution**: Dynamically parses dependency modules to discover exported types, replacing hardcoded type lists with accurate, package-specific resolution.

2. **Module Alias Expansion**: Expands module aliases (e.g., `D.Decoder` → `Json.Decode.Decoder`) while respecting ambiguous cases where both alias and direct import exist.

3. **Type Alias Expansion**: Expands type aliases when function implementations have more parameters than type signatures suggest, matching Elm compiler behavior.

Together, these ensure that generated documentation is consistent with the official Elm compiler output and works correctly with arbitrary packages.
