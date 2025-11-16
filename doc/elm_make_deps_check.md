# Elm Make Dependency Checking Algorithm

## Overview

This document describes the exact algorithm that `elm make` uses to check dependencies and cached dependencies, based on analysis of the Haskell source code in the Elm compiler.

## The "INCOMPATIBLE DEPENDENCIES" Error

The error message you encountered:
```
elm make examples/src/Main.elm
Dependencies ready!         
-- INCOMPATIBLE DEPENDENCIES ------------------------------------------ elm.json

The dependencies in your elm.json are not compatible.

Did you change them by hand? Try to change it back! It is much more reliable to
add dependencies with elm install or the dependency management tool in
elm reactor.
```

This error is generated at: `builder/src/Reporting/Exit.hs:1276-1287`

## Complete Algorithm Flow

### 1. Entry Point (`terminal/src/Make.hs`)

When you run `elm make <file>`, the process begins:

```haskell
run :: [FilePath] -> Flags -> IO ()
run paths flags =
  do  style <- getStyle report
      maybeRoot <- Stuff.findRoot
      case maybeRoot of
        Just root -> runHelp root paths style flags
        Nothing   -> return $ Left $ Exit.MakeNoOutline

runHelp :: FilePath -> [FilePath] -> Reporting.Style -> Flags -> IO (Either Exit.Make ())
runHelp root paths style flags =
  BW.withScope $ \scope ->
  Stuff.withRootLock root $ Task.run $
  do  details <- Task.eio Exit.MakeBadDetails (Details.load style scope root)
      -- ... continue with build
```

**Steps:**
- Finds project root containing `elm.json`
- Acquires lock on project
- Loads or generates `Details`

### 2. Loading/Generating Details (`builder/src/Elm/Details.hs:163-176`)

```haskell
load :: Reporting.Style -> BW.Scope -> FilePath -> IO (Either Exit.Details Details)
load style scope root =
  do  newTime <- File.getTime (root </> "elm.json")
      maybeDetails <- File.readBinary (Stuff.details root)
      case maybeDetails of
        Nothing ->
          generate style scope root newTime
        
        Just details@(Details oldTime _ buildID _ _ _) ->
          if oldTime == newTime
          then return (Right details { _buildID = buildID + 1 })
          else generate style scope root newTime
```

**Cache Check Logic:**
1. Get modification time of `elm.json`
2. Try to read cached details from `elm-stuff/<version>/d.dat` (binary file)
3. **If no cache exists** → call `generate`
4. **If cache exists but timestamp differs** → call `generate`
5. **If cache exists and timestamp matches** → reuse cache (increment build ID)

**Important:** The timestamp check is a simple equality comparison. Any modification to `elm.json` invalidates the cache.

### 3. Generate Details (`builder/src/Elm/Details.hs:181-193`)

```haskell
generate :: Reporting.Style -> BW.Scope -> FilePath -> File.Time -> IO (Either Exit.Details Details)
generate style scope root time =
  Reporting.trackDetails style $ \key ->
    do  result <- initEnv key scope root
        case result of
          Left exit -> return (Left exit)
          Right (env, outline) ->
            case outline of
              Outline.Pkg pkg -> Task.run (verifyPkg env time pkg)
              Outline.App app -> Task.run (verifyApp env time app)
```

**Initialize Environment:**
```haskell
initEnv :: Reporting.DKey -> BW.Scope -> FilePath -> IO (Either Exit.Details (Env, Outline.Outline))
initEnv key scope root =
  do  mvar <- fork Solver.initEnv
      eitherOutline <- Outline.read root
      case eitherOutline of
        Left problem -> return $ Left $ Exit.DetailsBadOutline problem
        Right outline ->
          do  maybeEnv <- readMVar mvar
              case maybeEnv of
                Left problem -> return $ Left $ Exit.DetailsCannotGetRegistry problem
                Right (Solver.Env cache manager connection registry) ->
                  return $ Right (Env key scope root cache manager connection registry, outline)
```

**Parallel Operations:**
1. **Fork thread** to initialize solver environment (loads/updates registry)
2. **Main thread** reads and parses `elm.json`
3. **Wait** for both to complete

**Registry Loading (`builder/src/Deps/Registry.hs`):**
- Reads cached registry from `~/.elm/<version>/packages/registry.dat`
- If no cache: fetches from `https://package.elm-lang.org/all-packages`
- If cache exists: tries to update from `/all-packages/since/<count>`
- **The "Dependencies ready!" message appears here** - it means registry is loaded, NOT that dependencies are valid!

### 4. Application Verification (`builder/src/Elm/Details.hs:247-257`)

For application projects (type: "application" in elm.json):

```haskell
verifyApp :: Env -> File.Time -> Outline.AppOutline -> Task Details
verifyApp env time outline@(Outline.AppOutline elmVersion srcDirs direct _ _ _) =
  if elmVersion == V.compiler
  then
    do  stated <- checkAppDeps outline
        actual <- verifyConstraints env (Map.map Con.exactly stated)
        if Map.size stated == Map.size actual
          then verifyDependencies env time (ValidApp srcDirs) actual direct
          else Task.throw $ Exit.DetailsHandEditedDependencies
  else
    Task.throw $ Exit.DetailsBadElmInAppOutline elmVersion
```

**Validation Steps:**
1. Check Elm version matches compiler
2. Check dependency structure (`checkAppDeps`)
3. Verify constraints with solver
4. Verify all packages can be built

### 5. Check App Dependencies (`builder/src/Elm/Details.hs:260-265`)

```haskell
checkAppDeps :: Outline.AppOutline -> Task (Map.Map Pkg.Name V.Version)
checkAppDeps (Outline.AppOutline _ _ direct indirect testDirect testIndirect) =
  do  x <- union allowEqualDups indirect testDirect
      y <- union noDups direct testIndirect
      union noDups x y
```

**Critical Validation:**

The structure of an application's `elm.json`:
```json
{
  "dependencies": {
    "direct": { ... },
    "indirect": { ... }
  },
  "test-dependencies": {
    "direct": { ... },
    "indirect": { ... }
  }
}
```

**Merge Rules:**
1. **Merge `indirect` ∪ `test.direct`** → allows duplicates if versions match exactly
2. **Merge `direct` ∪ `test.indirect`** → no duplicates allowed
3. **Merge results of 1 and 2** → no duplicates allowed

**Throws `DetailsHandEditedDependencies` if:**
- A package appears in both `direct` and `test.indirect` (even if same version)
- A package appears in both `indirect` and `test.direct` with different versions
- Any other structural violation

### 6. Verify Constraints (`builder/src/Elm/Details.hs:270-276`)

```haskell
verifyConstraints :: Env -> Map.Map Pkg.Name Con.Constraint -> Task (Map.Map Pkg.Name Solver.Details)
verifyConstraints (Env _ _ _ cache _ connection registry) constraints =
  do  result <- Task.io $ Solver.verify cache connection registry constraints
      case result of
        Solver.Ok details        -> return details
        Solver.NoSolution        -> Task.throw $ Exit.DetailsNoSolution  -- YOUR ERROR
        Solver.NoOfflineSolution -> Task.throw $ Exit.DetailsNoOfflineSolution
        Solver.Err exit          -> Task.throw $ Exit.DetailsSolverProblem exit
```

**This is where "INCOMPATIBLE DEPENDENCIES" originates!**

For applications, constraints are created with `Con.exactly`:
```haskell
actual <- verifyConstraints env (Map.map Con.exactly stated)
```

Each package version in your `elm.json` becomes an exact constraint (e.g., "elm/core" → exactly 1.0.4).

### 7. The Constraint Solver (`builder/src/Deps/Solver.hs`)

#### Solver Data Structures

```haskell
data State =
  State
    { _cache :: Stuff.PackageCache
    , _connection :: Connection
    , _registry :: Registry.Registry
    , _constraints :: Map.Map (Pkg.Name, V.Version) Constraints
    }

data Constraints =
  Constraints
    { _elm :: C.Constraint
    , _deps :: Map.Map Pkg.Name C.Constraint
    }

data Goals =
  Goals
    { _pending :: Map.Map Pkg.Name C.Constraint  -- Unsolved constraints
    , _solved :: Map.Map Pkg.Name V.Version      -- Chosen versions
    }
```

#### Main Solver Algorithm (`exploreGoals`, lines 211-222)

```haskell
exploreGoals :: Goals -> Solver (Map.Map Pkg.Name V.Version)
exploreGoals (Goals pending solved) =
  case Map.minViewWithKey pending of
    Nothing ->
      return solved  -- All constraints satisfied!
    
    Just ((name, constraint), otherPending) ->
      do  let goals1 = Goals otherPending solved
          let addVsn = addVersion goals1 name
          (v,vs) <- getRelevantVersions name constraint
          goals2 <- oneOf (addVsn v) (map addVsn vs)
          exploreGoals goals2
```

**Algorithm (Backtracking Constraint Propagation):**

1. **Base case:** If no pending constraints, return solved versions
2. **Pick next package:** Take minimum package name from pending
3. **Get versions:** Find all versions from registry satisfying the constraint
4. **Try versions:** Starting with newest, try each version:
   - Add version to solved
   - Fetch that version's `elm.json` to get its dependencies
   - Add those dependencies as new constraints
   - Recursively solve
5. **Backtrack:** If a version fails, try the next older version
6. **Fail:** If all versions fail, return `NoSolution`

#### Adding a Version (`addVersion`, lines 225-234)

```haskell
addVersion :: Goals -> Pkg.Name -> V.Version -> Solver Goals
addVersion (Goals pending solved) name version =
  do  (Constraints elm deps) <- getConstraints name version
      if C.goodElm elm
        then
          do  newPending <- foldM (addConstraint solved) pending (Map.toList deps)
              return (Goals newPending (Map.insert name version solved))
        else
          backtrack
```

**Process:**
1. Get constraints for this package@version (from cache or download `elm.json`)
2. Check if package supports current Elm version
3. For each dependency of this package:
   - Call `addConstraint` to merge with existing constraints
4. If successful, add version to solved and return new goals
5. If any step fails, backtrack to try next version

#### Constraint Merging (`addConstraint`, lines 236-254)

```haskell
addConstraint :: Map.Map Pkg.Name V.Version -> Map.Map Pkg.Name C.Constraint -> (Pkg.Name, C.Constraint) -> Solver (Map.Map Pkg.Name C.Constraint)
addConstraint solved unsolved (name, newConstraint) =
  case Map.lookup name solved of
    Just version ->
      if C.satisfies newConstraint version
      then return unsolved
      else backtrack  -- Already solved version doesn't satisfy new constraint!
    
    Nothing ->
      case Map.lookup name unsolved of
        Nothing ->
          return $ Map.insert name newConstraint unsolved
        
        Just oldConstraint ->
          case C.intersect oldConstraint newConstraint of
            Nothing ->
              backtrack  -- Constraints don't overlap!
            
            Just mergedConstraint ->
              if oldConstraint == mergedConstraint
              then return unsolved
              else return (Map.insert name mergedConstraint unsolved)
```

**Conflict Detection:**
1. **If package already solved:** Check if chosen version satisfies new constraint
   - If yes: continue
   - If no: **BACKTRACK** (triggers trying different version)
2. **If package pending:** Intersect old and new constraints
   - If intersection exists: use merged constraint
   - If no intersection: **BACKTRACK** (no valid version exists)

#### Constraint Intersection (`compiler/src/Elm/Constraint.hs:117-134`)

Constraints are version ranges:
```haskell
data Constraint = Range V.Version Op Op V.Version
data Op = Less | LessOrEqual
```

Example: `1.0.0 <= v <= 2.0.0`

```haskell
intersect :: Constraint -> Constraint -> Maybe Constraint
intersect (Range lo lop hop hi) (Range lo_ lop_ hop_ hi_) =
  let
    (newLo, newLop) =
      case compare lo lo_ of
        LT -> (lo_, lop_)      -- Take higher lower bound
        EQ -> (lo, stricter)
        GT -> (lo, lop)
    
    (newHi, newHop) =
      case compare hi hi_ of
        LT -> (hi, hop)        -- Take lower upper bound
        EQ -> (hi, stricter)
        GT -> (hi_, hop_)
  in
    if newLo <= newHi then
      Just (Range newLo newLop newHop newHi)
    else
      Nothing  -- No overlap! Constraints are incompatible!
```

**Examples:**
- `[1.0.0, 2.0.0]` ∩ `[1.5.0, 3.0.0]` = `[1.5.0, 2.0.0]` ✓
- `[1.0.0, 1.5.0]` ∩ `[2.0.0, 3.0.0]` = `Nothing` ✗ (no overlap)
- `exactly 1.0.0` ∩ `exactly 2.0.0` = `Nothing` ✗ (your error case!)

### 8. Getting Constraints (`getConstraints`, lines 273-318)

```haskell
getConstraints :: Pkg.Name -> V.Version -> Solver Constraints
getConstraints pkg vsn =
  Solver $ \state@(State cache connection registry cDict) ok back err ->
    do  let key = (pkg, vsn)
        case Map.lookup key cDict of
          Just cs -> ok state cs back  -- Already fetched
          
          Nothing ->
            do  let home = Stuff.package cache pkg vsn
                let path = home </> "elm.json"
                outlineExists <- File.exists path
                if outlineExists
                  then
                    -- Read cached elm.json
                    bytes <- File.readUtf8 path
                    case D.fromByteString constraintsDecoder bytes of
                      Right cs -> ok (toNewState cs) cs back
                      Left  _  -> err (Exit.SolverBadCacheData pkg vsn)
                else
                  case connection of
                    Offline -> back state  -- Can't fetch, backtrack
                    Online manager ->
                      -- Download elm.json from package.elm-lang.org
                      let url = Website.metadata pkg vsn "elm.json"
                      result <- Http.get manager url [] id (return . Right)
                      case result of
                        Right body ->
                          case D.fromByteString constraintsDecoder body of
                            Right cs ->
                              -- Cache for future use
                              Dir.createDirectoryIfMissing True home
                              File.writeUtf8 path body
                              ok (toNewState cs) cs back
                            Left _ -> err (Exit.SolverBadHttpData pkg vsn url)
                        Left httpProblem -> err (Exit.SolverBadHttp pkg vsn httpProblem)
```

**Process:**
1. Check if constraints for this package@version are cached in memory
2. If not, check if `elm.json` exists in `~/.elm/<version>/packages/<author>/<name>/<version>/`
3. If cached, read and parse it
4. If not cached and online, download from `https://package.elm-lang.org/packages/<author>/<name>/<version>/elm.json`
5. Parse to extract elm version constraint and dependency constraints
6. Cache in memory and on disk
7. If any step fails, backtrack or error

### 9. Verify Dependencies (`builder/src/Elm/Details.hs:314-341`)

After solver succeeds, verify each package can be built:

```haskell
verifyDependencies :: Env -> File.Time -> ValidOutline -> Map.Map Pkg.Name Solver.Details -> Map.Map Pkg.Name a -> Task Details
verifyDependencies env@(Env key scope root cache _ _ _) time outline solution directDeps =
  Task.eio id $
  do  Reporting.report key (Reporting.DStart (Map.size solution))
      mvar <- newEmptyMVar
      mvars <- Stuff.withRegistryLock cache $
        Map.traverseWithKey (\k v -> fork (verifyDep env mvar solution k v)) solution
      putMVar mvar mvars
      deps <- traverse readMVar mvars
      case sequence deps of
        Left _ ->
          -- Some package failed to build
          return $ Left $ Exit.DetailsBadDeps ...
        
        Right artifacts ->
          -- All packages built successfully
          -- Write cached interfaces, objects, and details
          BW.writeBinary scope (Stuff.objects    root) objs
          BW.writeBinary scope (Stuff.interfaces root) ifaces
          BW.writeBinary scope (Stuff.details    root) details
          return (Right details)
```

**Parallel verification:**
1. For each package in solution, fork a thread to verify it
2. Each thread calls `verifyDep`
3. Wait for all to complete
4. If any fail, report error
5. If all succeed, cache the results

### 10. Verify Single Dependency (`verifyDep`, lines 379-410)

```haskell
verifyDep :: Env -> MVar (Map.Map Pkg.Name (MVar Dep)) -> Map.Map Pkg.Name Solver.Details -> Pkg.Name -> Solver.Details -> IO Dep
verifyDep (Env key _ _ cache manager _ _) depsMVar solution pkg details@(Solver.Details vsn directDeps) =
  do  let fingerprint = Map.intersectionWith (\(Solver.Details v _) _ -> v) solution directDeps
      exists <- Dir.doesDirectoryExist (Stuff.package cache pkg vsn </> "src")
      if exists
        then
          -- Package already downloaded
          maybeCache <- File.readBinary (Stuff.package cache pkg vsn </> "artifacts.dat")
          case maybeCache of
            Nothing ->
              build key cache depsMVar pkg details fingerprint Set.empty
            
            Just (ArtifactCache fingerprints artifacts) ->
              if Set.member fingerprint fingerprints
                then return (Right artifacts)  -- Cache hit!
                else build key cache depsMVar pkg details fingerprint fingerprints
        else
          -- Download package
          result <- downloadPackage cache manager pkg vsn
          case result of
            Left problem -> return $ Left $ Just $ Exit.BD_BadDownload pkg vsn problem
            Right () -> build key cache depsMVar pkg details fingerprint Set.empty
```

**Fingerprint-based caching:**

A "fingerprint" is the set of exact versions of a package's direct dependencies. For example:
```
Package: elm/html@1.0.0
Direct deps: {elm/core → 1.0.4, elm/json → 1.1.2, elm/virtual-dom → 1.0.2}
Fingerprint: {elm/core → 1.0.4, elm/json → 1.1.2, elm/virtual-dom → 1.0.2}
```

**Cache invalidation:**
- Each package can have multiple cached builds with different fingerprints
- If dependencies change (e.g., `elm/core` upgraded 1.0.4 → 1.0.5), new fingerprint
- Package must be rebuilt with new fingerprint
- Old fingerprint cache remains valid for other projects

## Why "INCOMPATIBLE DEPENDENCIES" Occurs

The `DetailsNoSolution` error happens when the solver exhausts all possibilities:

### Common Scenarios

#### 1. Exact Constraint Conflicts

Your `elm.json` specifies exact versions:
```json
{
  "dependencies": {
    "direct": {
      "elm/core": "1.0.4",
      "some/package": "2.0.0"
    },
    "indirect": {
      "elm/json": "1.1.2"
    }
  }
}
```

When solver processes `some/package@2.0.0`, it reads its `elm.json`:
```json
{
  "dependencies": {
    "elm/json": "1.0.0 <= v < 2.0.0"
  }
}
```

**Conflict:**
- Your constraint: `elm/json` exactly 1.1.2
- Package's constraint: `elm/json` 1.0.0 ≤ v < 2.0.0
- Intersection: exactly 1.1.2 ✓ (this would work)

But if package requires:
```json
{
  "dependencies": {
    "elm/json": "1.0.0 <= v < 1.1.0"
  }
}
```

**Conflict:**
- Your constraint: exactly 1.1.2
- Package's constraint: [1.0.0, 1.1.0)
- Intersection: `Nothing` ✗ **NoSolution!**

#### 2. Transitive Diamond Dependency Conflicts

```
Your Project
├── package-a@1.0.0
│   └── shared@1.0.0
└── package-b@1.0.0
    └── shared@2.0.0
```

Solver tries:
1. Add `package-a@1.0.0` → requires `shared@1.0.0`
2. Add `shared@1.0.0` to constraints
3. Add `package-b@1.0.0` → requires `shared@2.0.0`
4. Try to merge constraints: `exactly 1.0.0` ∩ `exactly 2.0.0` = `Nothing`
5. **BACKTRACK** - try older version of `package-b`
6. If all versions of `package-b` conflict → **NoSolution!**

#### 3. Non-existent Versions

Your `elm.json` lists:
```json
"dependencies": {
  "direct": {
    "elm/core": "1.0.99"
  }
}
```

But registry only has: `1.0.0, 1.0.1, 1.0.2, 1.0.3, 1.0.4, 1.0.5`

**Result:**
- `getRelevantVersions` filters versions satisfying `exactly 1.0.99`
- Returns empty list
- Solver backtracks immediately
- **NoSolution!**

#### 4. Manual Editing Mistakes

Most common cause! The error message specifically mentions this:

> Did you change them by hand?

**Example:**
- You run `elm install some/package`
- Elm solver computes valid solution, writes to `elm.json`
- You manually edit `elm.json` to "fix" something
- You change one version without understanding transitive dependencies
- Next `elm make` finds the constraints are now impossible

### Why "Dependencies ready!" is Misleading

The message appears in this sequence:

```
1. Start elm make
2. Find elm.json
3. Load registry from cache/network
   → Print "Dependencies ready!" (registry loaded successfully)
4. Parse elm.json
5. Run constraint solver
   → Solver returns NoSolution
   → Print "INCOMPATIBLE DEPENDENCIES"
```

The message only means "I have the registry" not "Your dependencies are valid"!

## Debugging Process

### Step 1: Check for Manual Edits

Compare your `elm.json` with git history or backup:
```bash
git diff elm.json
```

### Step 2: Try Fresh Solve

Delete indirect dependencies and let Elm recompute:
```json
{
  "dependencies": {
    "direct": { /* keep this */ },
    "indirect": {}  // Delete all
  }
}
```

Run `elm make` - if it works, Elm will regenerate valid `indirect` section.

### Step 3: Binary Search Dependencies

Comment out half your direct dependencies, run `elm make`:
- If succeeds: problem is in commented half
- If fails: problem is in uncommented half

Repeat until you find the conflicting package.

### Step 4: Check Individual Package Constraints

For each package in your direct dependencies, check its elm.json:
```bash
# View package constraints
cat ~/.elm/0.19.1/packages/author/name/version/elm.json
```

Look for conflicts in their dependency requirements.

### Step 5: Use `elm install` Instead

Rather than manual editing:
```bash
# Remove from elm.json first, then:
elm install author/package

# This runs the solver and ensures valid state
```

## Cache Files and Locations

### Project Cache (`elm-stuff/<version>/`)

```
elm-stuff/0.19.1/
├── lock                 # Project lock file
├── d.dat               # Details cache (binary)
├── i.dat               # Interfaces cache (binary)
├── o.dat               # Objects cache (binary)
└── Main.elmi           # Compiled module interface
└── Main.elmo           # Compiled module objects
```

**Details cache (`d.dat`)** contains:
```haskell
data Details =
  Details
    { _outlineTime :: File.Time      -- elm.json modification time
    , _outline :: ValidOutline       -- Parsed elm.json
    , _buildID :: BuildID            -- Incremented each build
    , _locals :: Map.Map ModuleName.Raw Local
    , _foreigns :: Map.Map ModuleName.Raw Foreign
    , _extras :: Extras              -- Cached interfaces/objects
    }
```

### Global Cache (`~/.elm/<version>/packages/`)

```
~/.elm/0.19.1/packages/
├── registry.dat                           # Binary registry cache
├── author/
│   └── package/
│       └── version/
│           ├── elm.json                   # Downloaded package metadata
│           ├── artifacts.dat              # Built artifacts with fingerprints
│           ├── src/                       # Downloaded source code
│           ├── docs.json                  # Generated documentation
│           └── README.md
```

**Artifacts cache (`artifacts.dat`)** contains:
```haskell
data ArtifactCache =
  ArtifactCache
    { _fingerprints :: Set.Set Fingerprint  -- All valid fingerprints
    , _artifacts :: Artifacts               -- Compiled interfaces/objects
    }

type Fingerprint = Map.Map Pkg.Name V.Version  -- Exact deps versions
```

Multiple fingerprints allow one package build to serve multiple projects with different dependency versions.

## Summary

The Elm dependency checking algorithm:

1. **Loads registry** of all available packages and versions
2. **Parses elm.json** to get your specified dependencies
3. **Validates structure** (no duplicates between direct/indirect/test sections)
4. **Runs constraint solver** using backtracking:
   - Converts exact versions to constraints
   - For each package, fetches its elm.json to get its constraints
   - Recursively merges constraints via intersection
   - Tries all version combinations (newest first)
   - Returns `NoSolution` if all combinations fail
5. **Verifies builds** by downloading and compiling each package
6. **Caches results** using fingerprints for fast rebuilds

The "INCOMPATIBLE DEPENDENCIES" error means the constraint solver exhausted all possible version combinations without finding a valid solution. This typically happens due to:
- Manual edits creating impossible constraints
- Transitive dependency conflicts
- Non-existent versions
- Packages with incompatible Elm version requirements

Always use `elm install` instead of manual editing to maintain valid dependency state.
