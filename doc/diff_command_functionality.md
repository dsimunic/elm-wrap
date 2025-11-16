# Elm Diff Command Functionality

## Overview

The `elm diff` command compares API changes between Elm package versions. It is implemented in Haskell within the Elm compiler codebase and requires internet access to function, as it fetches the latest package registry and documentation from `https://package.elm-lang.org`.

## Offline Limitations

The command cannot operate in offline mode because it always attempts to fetch the latest package registry:

1. **Registry Fetching**: `Registry.latest` retrieves the complete package list from the registry website. It checks for cached data but attempts incremental updates, failing if network access is unavailable.

2. **Documentation Retrieval**: For version comparisons, it fetches `docs.json` files containing API documentation. While these are cached after first download, the registry update requirement prevents offline operation.

3. **Version Validation**: All operations require up-to-date registry information to validate package versions and availability.

## Diffing Process Flow

### 1. Environment Setup
- Detects project root directory
- Initializes HTTP manager and package cache
- **Fetches latest registry** (blocks offline usage)
- Validates Elm project structure

### 2. Command Variants
- `elm diff`: Compare current code against latest published version
- `elm diff <version>`: Compare current code against specific version
- `elm diff <pkg> <v1> <v2>`: Compare two published versions of a package

### 3. Documentation Acquisition
- **Published versions**: Downloads `docs.json` from package registry (cached locally)
- **Current code**: Builds Elm project and extracts documentation from compilation artifacts

### 4. API Comparison
Compares documentation structures at the module level:
- **Modules**: Added, removed, or modified
- **Types**: Custom types, type aliases, records
- **Values**: Functions and constants with type signatures
- **Operators**: Binary operators with precedence/associativity

### 5. Change Classification
Categorizes changes by severity:
- **PATCH**: Internal changes, documentation updates
- **MINOR**: New features (added exports)
- **MAJOR**: Breaking changes (removed/changed exports)

## How Type Equivalence Works

The command uses structural equivalence checking on Elm's resolved type representations, not the full type checking machinery. It performs pattern matching on `Type.Type` data structures with alpha-renaming support.

### Structural Pattern Matching

The `diffType` function recursively compares type constructors:

```haskell
diffType :: Type.Type -> Type.Type -> Maybe [(Name.Name,Name.Name)]
diffType oldType newType =
  case (oldType, newType) of
    (Type.Var oldName, Type.Var newName) ->
      Just [(oldName, newName)]  -- Allow variable renaming
    
    (Type.Lambda a b, Type.Lambda a' b') ->
      (++) <$> diffType a a' <*> diffType b b'
    
    (Type.Type oldName oldArgs, Type.Type newName newArgs) ->
      if not (isSameName oldName newName) || length oldArgs /= length newArgs then
        Nothing
      else
        concat <$> zipWithM diffType oldArgs newArgs
    
    -- Similar patterns for records, tuples, units
```

### Alpha-Renaming of Type Variables

Type variables can be renamed while maintaining equivalence:

```elm
-- Considered equivalent:
type alias Config a = { field : a }
type alias Config b = { field : b }

-- Not equivalent (special variable names):
type alias Config a = { field : a }
type alias Config number = { field : number }
```

### Type Variable Compatibility

Renamed variables must maintain compatible categories:

```haskell
compatibleVars :: (Name.Name, Name.Name) -> Bool
compatibleVars (old, new) =
  case (categorizeVar old, categorizeVar new) of
    (CompAppend, CompAppend) -> True    -- comparable + appendable
    (Comparable, Comparable) -> True    -- ord + eq  
    (Appendable, Appendable) -> True    -- semigroup
    (Number    , Number    ) -> True    -- num
    (Number    , Comparable) -> True    -- num implies comparable
    (_, Var) -> True                    -- concrete to variable ok
    (_, _) -> False
```

### Key Characteristics

- **No unification**: Doesn't solve constraints or check type relationships
- **No inference**: Works on already-resolved types from `docs.json`
- **No compilation during diff**: Types extracted from build artifacts
- **Pattern-based**: Pure structural matching with limited semantic awareness

This approach provides semantic equivalence checking suitable for API diffing without the complexity of full type checking, focusing on structural compatibility rather than deep type relationships.