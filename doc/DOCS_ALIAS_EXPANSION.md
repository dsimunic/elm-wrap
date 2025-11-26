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

2. **Aliased imports**: Makes a module available under an alternative name
   ```elm
   import Json.Schema.Validation as Validation
   import Html.Attributes as Html
   ```

A module can be both directly imported AND aliased simultaneously:
```elm
import Html              -- Direct import
import Html.Attributes as Html  -- Alias that shadows the direct import
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

2. **Check if `Prefix` is also directly imported**:
   ```c
   bool is_direct = is_directly_imported(direct_imports, "Prefix");
   ```

3. **Apply expansion rule**:
   ```c
   bool should_expand = (full_module != NULL) && !is_direct;
   ```

   - If `should_expand` is true: Replace `Prefix` with the full module name
   - Otherwise: Keep `Prefix` as-is

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

### Example 2: Alias Not Expanded (Ambiguity)

Source code:
```elm
import Html                    -- Direct import
import Html.Attributes as Html -- Alias that shadows

dropdown : List (Html.Attribute msg) -> Html msg
```

Processing for `Html.Attribute`:
- `Html` is an alias → `lookup_module_alias` returns `"Html.Attributes"`
- `Html` IS directly imported → `is_directly_imported` returns `true`
- `should_expand` = `false` → Do NOT expand the alias

Generated documentation:
```elm
dropdown : List (Html.Attribute msg) -> Html msg
```

Processing for `Html msg`:
- `Html` appears without a dot suffix, so it's treated as a type reference
- The type qualification rules handle this separately (qualifying to `Html.Html`)

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

When processing import statements, the algorithm tracks direct imports:

```c
if (strcmp(import_child_type, "upper_case_qid") == 0) {
    module_name = get_node_text(import_child, source_code);

    // Track as directly imported
    add_direct_import(direct_imports, module_name);

    // Also track the base module if this is a qualified name
    // e.g., "Json.Decode" adds both "Json.Decode" and "Json"
    char *dot = strchr(module_name, '.');
    if (dot) {
        // Add base module (everything before first dot)
        add_direct_import(direct_imports, base_module);
    }
}
```

This ensures that both `Json.Decode` and `Json` are tracked when `import Json.Decode` appears.

## Rationale

The algorithm's behavior matches the Elm compiler's principle: **module aliases are a convenience for the programmer, but documentation should reflect the actual module structure**.

The exception is when there's ambiguity (a name is both an alias and a direct import). In this case, the direct import takes precedence to match the programmer's visible namespace at the type annotation site.

This ensures that:
1. Documentation is consistent with the Elm compiler's output
2. Type references are unambiguous and traceable to their source modules
3. The documentation matches what developers see in the source code
