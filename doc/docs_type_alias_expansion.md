# Type Alias Expansion in Documentation Generation

This document explains why type aliases get expanded in generated documentation when a function implementation has more parameters than the type signature suggests.

## The Problem

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

The generated `docs.json` shows the expanded type:
```json
"type": "(a -> Edn.Decode.Decoder b) -> Edn.Decode.Decoder a -> Edn.Decode.Context -> Edn.Edn -> Result.Result String.String b"
```

Instead of the expected:
```json
"type": "(a -> Edn.Decode.Decoder b) -> Edn.Decode.Decoder a -> Edn.Decode.Decoder b"
```

## Root Cause

This is **not a special case** but a natural consequence of how canonicalization processes typed function definitions. The expansion happens during the argument-type matching phase.

## The Algorithm

### Phase 1: Canonicalization of Typed Definitions

When canonicalizing a definition with a type annotation, the compiler must match source-level arguments against the declared type.

**Function: `gatherTypedArgs`**

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

**Function: `iteratedDealias`**

```
iteratedDealias(type):
    if type is TAlias(home, name, args, realType):
        expandedType = dealias(args, realType)  -- substitute type variables
        return iteratedDealias(expandedType)    -- recurse in case of nested aliases
    else:
        return type
```

### Phase 2: Storing the Canonical Definition

The canonical AST stores typed definitions as:
```
TypedDef(name, freeVars, typedArgs, body, resultType)
```

Where:
- `typedArgs` = list of (pattern, type) pairs for each source argument
- `resultType` = the remaining type after consuming all source arguments

### Phase 3: Reconstructing Type for Documentation

When generating documentation, the full type is reconstructed:

```
reconstructType(typedArgs, resultType):
    return foldr(TLambda, resultType, map(snd, typedArgs))
```

This rebuilds `arg1 -> arg2 -> ... -> resultType`.

### Phase 4: Extracting to Documentation Type

```
extractType(canType):
    case canType of:
        TLambda(arg, result) -> Lambda(extractType(arg), extractType(result))
        TAlias(home, name, args, _) -> Type(publicName(home, name), map(extractType, args))
        TType(home, name, args) -> Type(publicName(home, name), map(extractType, args))
        -- ... other cases
```

**Key observation:** `TAlias` nodes are preserved as type names, but raw `TLambda` chains (from dealiased types) become expanded arrow types.

## Worked Example

For `andThen fn decoder ctx edn`:

1. **Initial type:** `(a -> Decoder b) -> Decoder a -> Decoder b`

2. **Process `fn`:**
   - Current type: `(a -> Decoder b) -> Decoder a -> Decoder b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `(a -> Decoder b)`, remaining = `Decoder a -> Decoder b`

3. **Process `decoder`:**
   - Current type: `Decoder a -> Decoder b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `Decoder a`, remaining = `Decoder b`

4. **Process `ctx`:**
   - Current type: `Decoder b` (which is `TAlias ... (Context -> Edn -> Result String b)`)
   - `iteratedDealias` → `Context -> Edn -> Result String b` (a `TLambda`!)
   - Consume: argType = `Context`, remaining = `Edn -> Result String b`

5. **Process `edn`:**
   - Current type: `Edn -> Result String b`
   - `iteratedDealias` → already a `TLambda`
   - Consume: argType = `Edn`, remaining = `Result String b`

6. **Result:**
   - `typedArgs` = [(fn, `a -> Decoder b`), (decoder, `Decoder a`), (ctx, `Context`), (edn, `Edn`)]
   - `resultType` = `Result String b`

7. **Reconstruction:**
   - `(a -> Decoder b) -> Decoder a -> Context -> Edn -> Result String b`

The alias `Decoder b` was expanded because `iteratedDealias` unwrapped it to find the arrows for `ctx` and `edn`.

## Implementation Notes

To preserve type aliases in documentation:

1. **Option A: Use the original annotation type directly**
   - Store the original `Can.Forall` annotation alongside `TypedDef`
   - Use it for documentation instead of reconstructing from `typedArgs`

2. **Option B: Track alias boundaries during `gatherTypedArgs`**
   - When `iteratedDealias` expands an alias, wrap the consumed argument types to indicate they came from inside an alias
   - During reconstruction, re-wrap in the original alias

3. **Option C: Limit implementation parameters**
   - Require that implementation parameters exactly match the type signature's arrow count
   - Extra "curried" parameters would need explicit type annotations

The current behavior is technically correct (the types are equivalent), but Option A is likely the cleanest fix for documentation purposes.

## File References

- `compiler/src/Canonicalize/Expression.hs` - `gatherTypedArgs` function (lines ~489-510)
- `compiler/src/AST/Utils/Type.hs` - `iteratedDealias` and `dealias` functions
- `compiler/src/Elm/Docs.hs` - `addDef` and type reconstruction (lines ~577-586)
- `compiler/src/Elm/Compiler/Type/Extract.hs` - `fromType` and `extract` functions
