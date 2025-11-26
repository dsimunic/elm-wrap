# Documentation Type Alias Expansion Algorithm

## Overview

When generating Elm package documentation, `elm-wrap` must decide whether to expand module aliases in type signatures. This document describes the algorithm used to make this decision, which matches the behavior of the official Elm compiler.

## Module Import Types

Elm supports two types of module imports that are relevant to alias expansion:

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

**Important**: When a module is imported with an alias, the original module name is **no longer available**. The alias completely replaces the original name.

```elm
import Html.Attributes as Html

-- This works:
val : Html.Attribute msg

-- This FAILS with "I cannot find a `Html.Attributes` import":
val : Html.Attributes.Attribute msg
```

**Last import wins**: If the same module is imported multiple times, only the last import's form is available:

```elm
import Html.Attributes as Html
import Html.Attributes        -- Last import wins

n = Html.Attributes.name       -- PASS (direct import is now active)
n = Html.name                  -- FAIL (alias was overwritten)
```

## Data Structures

The algorithm maintains three data structures:

### 1. ImportMap
Tracks types that are explicitly exposed from imported modules:
```c
typedef struct {
    char *type_name;       // e.g., "Schema"
    char *module_name;     // e.g., "Json.Schema.Definitions"
} TypeImport;
```

### 2. ModuleAliasMap
Tracks module aliases:
```c
typedef struct {
    char *alias;           // e.g., "Validation"
    char *full_module;     // e.g., "Json.Schema.Validation"
} ModuleAlias;
```

### 3. DirectModuleImports
Tracks modules that are directly imported (not just via alias):
```c
typedef struct {
    char **modules;        // e.g., ["Html", "Json.Decode", "String"]
    int modules_count;
} DirectModuleImports;
```

## Alias Expansion Algorithm

When processing a qualified type reference like `Foo.Bar`, the algorithm determines whether `Foo` is:

1. **Already qualified**: If preceded by a dot (e.g., `Module.Foo.Bar`), keep as-is
2. **A module prefix**: If followed by a dot (e.g., `Foo.Bar`), apply alias expansion rules
3. **An unqualified type**: If standalone (e.g., `Foo`), apply type qualification rules

### Module Prefix Processing

For a module prefix `Prefix` in `Prefix.TypeName`:

1. **Check if `Prefix` is a module alias**:
   ```c
   const char *full_module = lookup_module_alias(alias_map, "Prefix");
   ```

2. **Check if `Prefix` is also directly imported** (via a separate import statement):
   ```c
   bool is_direct = is_directly_imported(direct_imports, "Prefix");
   ```

3. **Apply expansion rule**:
   ```c
   // Expand alias, unless the same name is also directly imported
   bool should_expand = (full_module != NULL) && !is_direct;
   ```

   - If `should_expand` is true: Replace `Prefix` with the full module name
   - Otherwise: Keep `Prefix` as-is

**Note**: Since aliased imports do NOT add the original module name to `direct_imports`, aliases are always expanded unless there's a separate direct import of a module with the same name as the alias.

## Examples

### Example 1: Alias Expansion (No Ambiguity)

Source code:
```elm
import Json.Schema.Validation as Validation

validate : Validation.ValidationOptions -> ...
```

Processing:
- `Validation` is an alias → `lookup_module_alias` returns `"Json.Schema.Validation"`
- `Validation` is NOT directly imported → `is_directly_imported` returns `false`
- `should_expand` = `true` → Expand the alias

Generated documentation:
```elm
validate : Json.Schema.Validation.ValidationOptions -> ...
```

### Example 2: Dual Import (Both Alias and Direct Import with Same Name)

Source code:
```elm
import Html                    -- Direct import of Html module
import Html.Attributes as Html -- Alias: Html -> Html.Attributes

dropdown : List (Html.Attribute msg) -> Html.Html msg
```

Processing for `Html.Attribute`:
- `Html` is an alias → `lookup_module_alias` returns `"Html.Attributes"`
- `Html` IS directly imported (the `Html` module) → `is_directly_imported` returns `true`
- `should_expand` = `false` → Do NOT expand the alias

Generated documentation:
```elm
dropdown : List (Html.Attribute msg) -> Html.Html msg
```

**Explanation**: Here, `Html` refers to the directly imported `Html` module, not the alias. Since `Html.Attribute` doesn't exist in the `Html` module, this would actually be a compile error in Elm. The algorithm preserves the programmer's code as-is, letting them see the issue.

### Example 3: No Alias

Source code:
```elm
import Html

view : Html msg
```

Processing:
- `Html` has no dot suffix → Not processed by alias expansion
- Type qualification rules apply instead

## Direct Import Tracking

When processing import statements, the algorithm tracks direct imports **only for imports without an alias**:

```c
// Only add to direct_imports if there's NO alias
// (In Elm, aliased modules are not available by their original name)
if (!has_as_clause) {
    add_direct_import(direct_imports, module_name);

    // Also track the base module if this is a qualified name
    // e.g., "Json.Decode" adds both "Json.Decode" and "Json"
    char *dot = strchr(module_name, '.');
    if (dot) {
        add_direct_import(direct_imports, base_module);
    }
} else {
    // Aliased import: remove from direct imports (last import wins)
    remove_direct_import(direct_imports, module_name);
}
```

The algorithm also handles **"last import wins"** semantics:
- When a direct import follows an aliased import of the same module, the alias is removed
- When an aliased import follows a direct import of the same module, the direct import is removed

```c
// Direct import: remove any existing alias for this module
remove_alias_for_module(alias_map, module_name);
add_direct_import(direct_imports, module_name);

// OR

// Aliased import: remove from direct imports
remove_direct_import(direct_imports, module_name);
add_module_alias(alias_map, module_alias, module_name);  // Updates existing alias
```

## Rationale

The algorithm's behavior matches the Elm compiler: **when a module is aliased, only the alias is valid in source code**, and **the last import of a module wins**.

The `is_directly_imported` check handles the case where both an alias and a direct import resolve to the same name (e.g., `import Html` and `import Html.Attributes as Html`). In this case, whichever was imported last takes precedence.

This ensures that:
1. Documentation is consistent with the Elm compiler's output
2. Type references are unambiguous and traceable to their source modules
3. Aliases are always expanded to their full module paths
