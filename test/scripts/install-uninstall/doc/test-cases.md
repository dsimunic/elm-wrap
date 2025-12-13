# Install and uninstall test cases

## Tools

`WRAP_OFFLINE_MODE=1` -- turns off downloading the latest registry. This is a MUST.
`wrap info -d` - shows installed packages 
`indexmaker` -- makes a registry.dat (v1 registry) from imaginary-package-registry.txt
`mkpkg` -- creates a packge in cache based on the definition in registry.

### Recommended test harness setup (offline + deterministic)

For ALL tests: 
- the environment MUST contain `WRAP_OFFLINE_MODE=1`
- create a temporary directory for temp elm_home and assign the path to `ELM_HOME` available to the test environment.
- pre-populate `$ELM_HOME/.../packages/...` using `mkpkg` from `imaginary-package-registry.txt`
- generate a registry.dat file from `imaginary-package-registry.txt` using indexmaker and add it to `$ELM_HOME/packages/`

Summary: All tests must operate with these two env variables set as above. That's a must. Likely the driver script
will ensure that the enviornment is passed to test execution script.


For each test (or group of dependent tests):
- create a temporary directory
- cd in that temporary directory
  - initialize the app or package using `wrap application init` or `wrap package init PACKAGE` (create_app)
  - confirm dependencies before running the test with `wrap info -d`
  - run tests per instructions
  - check dependencies with `wrap info -d`

Dependent tests:
- a test other tests will depend on are marked with KEEP at the end
- a test that requires kept test is marked with depends_on. This translates to:
  - do not change directory
  - expect that data created during the previous test is avialable


### Test packages

#### Already defined

- `wrap/package-single-no-deps` (single version, only depends on required `elm/core`)
- `wrap/package-3v-no-deps` (three major versions: `1.0.0`, `2.0.0`, `3.0.0`, only depends on `elm/core`)
- `wrap/package-single-1d` (single version, depends on `wrap/dep-single-no-deps`)
- `wrap/dep-single-no-deps` (single version, no deps beyond required `elm/core`)
- `wrap/package-3m-no-deps` (three versions within a single major, e.g. `1.0.0`, `1.1.0`, `1.2.0`)
- `wrap/package-3v-changing-deps`
  - `1.0.0` depends on `wrap/depA-single-no-deps`
  - `2.0.0` depends on `wrap/depB-single-no-deps`
  - `3.0.0` depends on `wrap/depC-single-no-deps`
- `wrap/depA-single-no-deps`, `wrap/depB-single-no-deps`, `wrap/depC-single-no-deps` (single version)
- `wrap/dep-shared-single-no-deps` (single version)
- `wrap/package-A-single-1d-shared` and `wrap/package-B-single-1d-shared`
  - both depend on `wrap/dep-shared-single-no-deps`
- `wrap/testdep-req-newer-prod-single`
  - used to test `wrap install --test` failing without `--upgrade-all` when production deps are pinned.
  - shape: depends on some prod package range that requires upgrading (e.g. requires `wrap/package-3v-no-deps 3.0.0 <= v < 4.0.0`).

- `wrap/package-from-file-single-no-deps` (single version)
  - used to test `--from-file` / `--from-url` install paths.
  - at path: test/data/packages/install-uninstall/wrap/package-from-file-single-no-deps
  

## install

    Usage: wrap install PACKAGE[@VERSION] [PACKAGE[@VERSION]...]

    Install packages for your Elm project.

    Examples:
        wrap install elm/html                     # Add elm/html to your project
        wrap install elm/html@1.0.0               # Add elm/html at specific version
        wrap install elm/html 1.0.0               # Same (single package only)
        wrap install elm/html elm/json elm/url    # Add multiple packages at once
        wrap install elm/html@1.0.0 elm/json      # Mix versioned and latest
        wrap install --test elm/json              # Add elm/json as a test dependency
        wrap install --major elm/html             # Upgrade elm/html to next major version
        wrap install --from-file ./pkg.zip elm/html  # Install from local file
        wrap install --from-url URL elm/html         # Install from URL

    Options:
        --test                             # Install as test dependency
        --upgrade-all                      # Allow upgrading production deps (with --test)
        --major PACKAGE                    # Allow major version upgrade for package (single package only)
        --from-file PATH PACKAGE           # Install from local file/directory (single package only)
        --from-url URL PACKAGE             # Install from URL (single package only)
        --local-dev [--from-path PATH] [PACKAGE]
                                            # Install package for local development
        uninstall --local-dev              # Remove current package from local-dev tracking
        -v, --verbose                      # Show progress reports (registry, connectivity)
        -q, --quiet                        # Suppress progress reports
        -y, --yes                          # Automatically confirm changes
        --help                             # Show this help


### Happy paths

### APP-01: single package install

create_app "$TEST_ROOT/app01" "app01"

command: wrap install wrap/package-single-no-deps

confirm: 
 - input present with latest version in direct dependencies
 - no changes to other deps

KEEP

### APP-01: single package uninstall

depends_on: APP-01: single package install

confirm: 
 - input present with latest version in direct dependencies

command: wrap package uninstall wrap/package-single-no-deps

confirm:
  - input no longer present in direct dependencies
  - no changes to other deps

### APP-02: install multiple packages at once

create_app "$TEST_ROOT/app02" "app02"

command: wrap install wrap/package-single-no-deps wrap/package-3v-no-deps

confirm: 
  - two new direct dependencies
  - wrap/package-single-no-deps 1.0.0
  - wrap/package-3v-no-deps 3.0.0

KEEP

### APP-02: uninstall multiple packages at once

depends_on: APP-02: install multiple packages at once 

confirm:
  - direct dependencies present: 
    - wrap/package-single-no-deps 1.0.0
    - wrap/package-3v-no-deps 3.0.0

command: wrap package uninstall wrap/package-single-no-deps wrap/package-3v-no-deps

confirm:
  - two less direct dependencies
  - dependencies not present:
    - wrap/package-single-no-deps 1.0.0
    - wrap/package-3v-no-deps 3.0.0
  - no other changes to dependencies

### APP-03: Install package with targeted version (PACKAGE@VERSION)

create_app "$TEST_ROOT/app03" "app03"

command: wrap install wrap/package-3v-no-deps@2.0.0

confirm: 
 - direct dependency present:
   - wrap/package-3v-no-deps 2.0.0
 - no other changes to dependencies

### APP-04: Install package with dependencies

create_app "$TEST_ROOT/app04" "app04"

command: wrap install wrap/package-single-1d

confirm:
 - direct dependency present:
   - wrap/package-single-1d 1.0.0
 - indirect dependency present:
   - wrap/dep-single-no-deps 1.0.0
 - no unrelated dependency changes

### APP-05: Move package from indirect to direct

create_app "$TEST_ROOT/app05" "app05"

command: wrap install wrap/dep-single-no-deps

confirm:
 - wrap/dep-single-no-deps becomes a **direct** dependency at 1.0.0
 - wrap/dep-single-no-deps is no longer listed as indirect
 - wrap/package-single-1d remains a direct dependency at 1.0.0
 - no version changes occur

### APP-07: Install multiple packages with mixed version specifications

create_app "$TEST_ROOT/app07" "app07"

command: wrap install wrap/package-3v-no-deps@2.0.0 wrap/package-single-no-deps wrap/package-single-1d

confirm:
 - direct dependencies present:
   - wrap/package-3v-no-deps 2.0.0
   - wrap/package-single-no-deps 1.0.0
   - wrap/package-single-1d 1.0.0
 - indirect dependencies include:
   - wrap/dep-single-no-deps 1.0.0
 - no other dependency changes

### APP-08: Re-install does not cross major without --major

create_app "$TEST_ROOT/app08" "app08"

steps:
 - command: wrap install wrap/package-3v-no-deps@2.0.0
 - confirm: direct dependency is exactly 2.0.0

command: wrap install wrap/package-3v-no-deps

confirm:
 - direct dependency remains at 2.0.0 (no cross-major upgrade)

### APP-09: Re-install crosses major with --major (single package)

depends_on: APP-08: Re-install does not cross major without --major

command: wrap install --major wrap/package-3v-no-deps

confirm:
 - direct dependency becomes 3.0.0
 - no unrelated dependency changes

### APP-10: Install as test dependency (--test)

create_app "$TEST_ROOT/app10" "app10"

command: wrap install --test wrap/package-single-no-deps

confirm:
 - package is added under test direct dependencies
 - package is NOT added under production direct dependencies

### APP-11: Install test dep requiring prod upgrade (requires --upgrade-all)

create_app "$TEST_ROOT/app11" "app11"

steps:
 - command: wrap install wrap/package-3v-no-deps@1.0.0
 - confirm: production direct contains wrap/package-3v-no-deps 1.0.0

command: wrap install --test wrap/testdep-req-newer-prod-single

confirm:
 - command fails with a message explaining production deps are pinned (expected)
 - dependencies unchanged

command: wrap install --test --upgrade-all wrap/testdep-req-newer-prod-single

confirm:
 - test dep is added under test direct
 - production deps are allowed to upgrade as needed (wrap/package-3v-no-deps moves to 3.0.0 in this scenario)

### PKG-01: Install single package to package type

create_package "$TEST_ROOT/pkg01" "testauthor/pkg01"

command: wrap package install wrap/package-single-no-deps

confirm:
 - in `elm.json`, the dependency is added under `dependencies` as a constraint range
   - e.g. `"wrap/package-single-no-deps": "1.0.0 <= v < 2.0.0"`
 - no other dependencies are added

### PKG-02: Install multiple packages to package type

create_package "$TEST_ROOT/pkg02" "testauthor/pkg02"

command: wrap package install wrap/package-single-no-deps wrap/package-3v-no-deps

confirm:
 - both packages are added under `dependencies` as constraint ranges
 - versions are constraints, not pinned exact versions
 - no transitive dependencies are added

### PKG-03: Install package with targeted version to package type

create_package "$TEST_ROOT/pkg03" "testauthor/pkg03"

command: wrap package install wrap/package-3v-no-deps@2.0.0

confirm:
 - dependency constraint lower bound reflects the requested version:
   - e.g. `"wrap/package-3v-no-deps": "2.0.0 <= v < 3.0.0"`
 - no other dependencies are changed

### PKG-04: Install package with dependencies to package type

create_package "$TEST_ROOT/pkg04" "testauthor/pkg04"

command: wrap package install wrap/package-single-1d

confirm:
 - only the requested package is added under `dependencies` as a constraint
 - `wrap/dep-single-no-deps` is NOT added to `elm.json` (it is a transitive dependency)
 - installation still succeeds (solver resolves transitives for download/cache)

### Unhappy paths

### ERR-01: Install non-existent package (should fail)

create_app "$TEST_ROOT/err01" "err01"

command: wrap install wrap/does-not-exist

confirm:
 - command fails
 - dependencies unchanged

### ERR-02: Install package with non-existent version (should fail)

create_app "$TEST_ROOT/err02" "err02"

command: wrap install wrap/package-3v-no-deps@9.9.9

confirm:
 - command fails (no compatible version found)
 - dependencies unchanged

### ERR-03: Install multiple packages with one non-existent (should fail)

create_app "$TEST_ROOT/err03" "err03"

command: wrap install wrap/package-single-no-deps wrap/does-not-exist

confirm:
 - command fails
 - dependencies unchanged (verify via `wrap application info -d`, no partial install)

### ERR-04: Install multiple packages with one having wrong version (should fail)

create_app "$TEST_ROOT/err04" "err04"

command: wrap install wrap/package-single-no-deps wrap/package-3v-no-deps@9.9.9

confirm:
 - command fails
 - dependencies unchanged (verify via `wrap application info -d`, no partial install)

## test uninstall

### APP-01: Uninstall single package

create_app "$TEST_ROOT/app01" "app01"

steps:
 - command: wrap install wrap/package-single-no-deps
 - confirm: direct deps contain wrap/package-single-no-deps 1.0.0

command: wrap package remove wrap/package-single-no-deps

confirm:
 - wrap/package-single-no-deps removed from direct deps
 - no other dependencies changed

### APP-02: Uninstall package removing orphaned dependencies

create_app "$TEST_ROOT/app02" "app02"

steps:
 - command: wrap install wrap/package-single-1d
 - confirm:
   - direct: wrap/package-single-1d 1.0.0
   - indirect: wrap/dep-single-no-deps 1.0.0

command: wrap package uninstall wrap/package-single-1d

confirm:
 - wrap/package-single-1d removed from direct deps
 - wrap/dep-single-no-deps removed from indirect deps (orphan cleanup)

### APP-03: Uninstall package keeping shared dependencies

create_app "$TEST_ROOT/app03" "app03"

steps:
 - command: wrap install wrap/package-A-single-1d-shared wrap/package-B-single-1d-shared
 - confirm:
   - both packages are direct deps
   - wrap/dep-shared-single-no-deps is present as an indirect dep

command: wrap package remove wrap/package-A-single-1d-shared

confirm:
 - removed package-A from direct deps
 - wrap/dep-shared-single-no-deps remains installed as indirect (still required by package-B)

### APP-04: Uninstall direct dependency that's still needed indirectly

create_app "$TEST_ROOT/app04" "app04"

steps:
 - command: wrap install wrap/package-single-1d
 - command: wrap install wrap/dep-single-no-deps
 - confirm: wrap/dep-single-no-deps is direct

command: wrap package remove wrap/dep-single-no-deps

confirm:
 - wrap/dep-single-no-deps is no longer direct
 - wrap/dep-single-no-deps remains indirect (still required by wrap/package-single-1d)

### APP-05: Uninstall test dependency

create_app "$TEST_ROOT/app05" "app05"

steps:
 - command: wrap install --test wrap/package-single-no-deps
 - confirm: package is present in test direct deps

command: wrap package remove wrap/package-single-no-deps

confirm:
 - package removed from test direct deps
 - production deps unchanged

### PKG-01: Uninstall single package from package type

create_package "$TEST_ROOT/pkg01" "testauthor/pkg01"

steps:
 - command: wrap package install wrap/package-single-no-deps
 - confirm: dependency constraint exists in package elm.json

command: wrap package remove wrap/package-single-no-deps

confirm:
 - dependency removed from `dependencies`

### PKG-02: Uninstall package from package type (with dependencies)

create_package "$TEST_ROOT/pkg02" "testauthor/pkg02"

steps:
 - command: wrap package install wrap/package-single-1d
 - confirm: only wrap/package-single-1d is in `dependencies` (constraint)

command: wrap package uninstall wrap/package-single-1d

confirm:
 - dependency removed from `dependencies`
 - no other constraints touched

### ERR-01: Uninstall non-existent package (should fail)

create_app "$TEST_ROOT/err01" "err01"

command: wrap package remove wrap/does-not-exist

confirm:
 - command fails
 - dependencies unchanged

### ERR-02: Uninstall package not in dependencies (should fail)

create_app "$TEST_ROOT/err02" "err02"

steps:
 - command: wrap install wrap/package-single-no-deps

command: wrap package remove wrap/package-3v-no-deps

confirm:
 - command fails (package not in dependencies)
 - dependencies unchanged

### ERR-03: Try to uninstall elm/core (should succeed - elm/core can be removed)

create_app "$TEST_ROOT/err03" "err03"

command: wrap package remove elm/core

confirm:
 - command succeeds
 - dependencies no longer list elm/core under direct dependencies

## test upgrade

### APP-01: Upgrade single package to latest minor version

create_app "$TEST_ROOT/app01" "app01"

steps:
 - command: wrap install wrap/package-3m-no-deps@1.0.0
 - confirm: direct dep is 1.0.0

command: wrap package upgrade wrap/package-3m-no-deps

confirm:
 - direct dep becomes 1.2.0 (latest within major 1)
 - does NOT jump to a new major

### APP-02: Upgrade all packages to latest minor versions

create_app "$TEST_ROOT/app02" "app02"

steps:
 - command: wrap install wrap/package-3m-no-deps@1.0.0 wrap/package-3v-no-deps@1.0.0

command: wrap package upgrade

confirm:
 - wrap/package-3m-no-deps becomes 1.2.0
 - wrap/package-3v-no-deps becomes 1.x latest within major 1 (still 1.0.0 in current 3v fixture)
 - no package crosses major

### APP-03: Upgrade with no argument (should upgrade all)

create_app "$TEST_ROOT/app03" "app03"

steps:
 - command: wrap install wrap/package-3m-no-deps@1.0.0

command: wrap package upgrade

confirm:
 - behaves like APP-02 for all packages in dependencies

### APP-04: Upgrade package with changing dependencies

create_app "$TEST_ROOT/app04" "app04"

steps:
 - command: wrap install wrap/package-3v-changing-deps@1.0.0
 - confirm: indirect deps include wrap/depA-single-no-deps

command: wrap package upgrade --major wrap/package-3v-changing-deps

confirm:
 - direct dep becomes 3.0.0
 - old indirect depA is removed if no longer needed
 - new indirect depC is added

### APP-05: Upgrade single package to latest major version (--major)

create_app "$TEST_ROOT/app05" "app05"

steps:
 - command: wrap install wrap/package-3v-no-deps@2.0.0

command: wrap package upgrade --major wrap/package-3v-no-deps

confirm:
 - direct dep becomes 3.0.0

### APP-06: Upgrade all packages to latest major versions (--major all)

create_app "$TEST_ROOT/app06" "app06"

steps:
 - command: wrap install wrap/package-3v-no-deps@1.0.0 wrap/package-3m-no-deps@1.0.0

command: wrap package upgrade --major all

confirm:
 - wrap/package-3v-no-deps becomes 3.0.0
 - wrap/package-3m-no-deps stays within major 1 (still 1.2.0)

### APP-07: Upgrade with --major and no package (should upgrade all)

create_app "$TEST_ROOT/app07" "app07"

steps:
 - command: wrap install wrap/package-3v-no-deps@1.0.0

command: wrap package upgrade --major

confirm:
 - behaves like `wrap package upgrade --major all`

### PKG-01: Upgrade single package to latest minor (package type)

create_package "$TEST_ROOT/pkg01" "testauthor/pkg01"

steps:
 - command: wrap package install wrap/package-3m-no-deps@1.0.0
 - confirm: constraint is `1.0.0 <= v < 2.0.0`

command: wrap package upgrade wrap/package-3m-no-deps

confirm:
 - constraint lower bound updates to `1.2.0 <= v < 2.0.0`

### PKG-02: Upgrade all packages to latest minor (package type)

create_package "$TEST_ROOT/pkg02" "testauthor/pkg02"

steps:
 - command: wrap package install wrap/package-3m-no-deps@1.0.0 wrap/package-3v-no-deps@1.0.0

command: wrap package upgrade

confirm:
 - constraints update for packages that have newer versions within the allowed major

### PKG-03: Upgrade to major version (package type)

create_package "$TEST_ROOT/pkg03" "testauthor/pkg03"

steps:
 - command: wrap package install wrap/package-3v-no-deps@1.0.0
 - confirm: constraint is `1.0.0 <= v < 2.0.0`

command: wrap package upgrade --major wrap/package-3v-no-deps

confirm:
 - constraint becomes `3.0.0 <= v < 4.0.0`

### ERR-01: Upgrade non-existent package (should fail)

create_app "$TEST_ROOT/err01" "err01"

command: wrap package upgrade wrap/does-not-exist

confirm:
 - command fails
 - dependencies unchanged

### ERR-02: Upgrade package not in dependencies (should fail)

create_app "$TEST_ROOT/err02" "err02"

steps:
 - command: wrap install wrap/package-single-no-deps

command: wrap package upgrade wrap/package-3v-no-deps

confirm:
 - command fails (not in dependencies)
 - dependencies unchanged

### EDGE-01: Upgrade when already at latest version

create_app "$TEST_ROOT/edge01" "edge01"

steps:
 - command: wrap install wrap/package-3v-no-deps@3.0.0

command: wrap package upgrade wrap/package-3v-no-deps

confirm:
 - reports no upgrades available (or no changes)
 - dependencies unchanged

### EDGE-02: Minor upgrade when major version available (shouldn't upgrade to major)

create_app "$TEST_ROOT/edge02" "edge02"

steps:
 - command: wrap install wrap/package-3v-no-deps@2.0.0

command: wrap package upgrade wrap/package-3v-no-deps

confirm:
 - remains at 2.0.0 (since only a major upgrade exists)
 - crossing to 3.0.0 only happens with `wrap package upgrade --major ...`

## test local dev

### LOCAL-01: Initialize package with local-dev and install to app

create_package "$TEST_ROOT/local-pkg1" "$LOCAL_PKG1"
create_app "$TEST_ROOT/app01" "app01"

steps:
 - register-only mode (from within the package directory):
   - cd "$TEST_ROOT/local-pkg1"
   - command: wrap install --local-dev
   - confirm: prints that local-dev package was registered (no application dependencies to modify)

 - install into application:
   - cd "$TEST_ROOT/app01"
   - command: wrap install --local-dev --from-path "$TEST_ROOT/local-pkg1" "$LOCAL_PKG1"
   - confirm:
     - dependencies now contain `$LOCAL_PKG1` as a direct dependency
     - `wrap application info` indicates it is using a local-dev package (tracking enabled)

### LOCAL-02: Install package with --local-dev --from-path

create_app "$TEST_ROOT/app02" "app02"

steps:
 - create_package "$TEST_ROOT/local-src2" "testauthor/local-src2" (or reuse LOCAL-01)
 - command: wrap install --local-dev --from-path "$TEST_ROOT/local-src2" testauthor/local-src2

confirm:
 - dependencies contain testauthor/local-src2 as direct dependency
 - package is cached via symlink (verify by `wrap application info` output)

### LOCAL-03: Dependency synchronization when local package changes

create_package "$TEST_ROOT/local-pkg3" "testauthor/local-pkg-three"
create_app "$TEST_ROOT/app03" "app03"

steps:
 - cd "$TEST_ROOT/local-pkg3"
 - command: wrap install --local-dev

 - cd "$TEST_ROOT/app03"
 - command: wrap install --local-dev --from-path "$TEST_ROOT/local-pkg3" testauthor/local-pkg-three
 - confirm: dependencies contain testauthor/local-pkg-three

 - mutate the local package's elm.json dependencies (e.g. add `wrap/package-single-no-deps` as a dependency)
 - cd "$TEST_ROOT/app03"
 - command: wrap install testauthor/local-pkg-three

confirm:
 - dependencies update to include any newly-required dependencies resolved from the local package
 - no network access occurs (offline registry/cache)

### LOCAL-04: Multiple applications depending on same local package

create_package "$TEST_ROOT/local-pkg4" "testauthor/local-pkg-four"
create_app "$TEST_ROOT/app04a" "app04a"
create_app "$TEST_ROOT/app04b" "app04b"

steps:
 - cd "$TEST_ROOT/local-pkg4" && command: wrap install --local-dev
 - cd "$TEST_ROOT/app04a" && command: wrap install --local-dev --from-path "$TEST_ROOT/local-pkg4" testauthor/local-pkg-four
 - cd "$TEST_ROOT/app04b" && command: wrap install --local-dev --from-path "$TEST_ROOT/local-pkg4" testauthor/local-pkg-four

confirm:
 - both apps have the local-dev dependency recorded in dependencies
 - `wrap application info` for each app shows local-dev tracking

steps:
 - change local package dependencies (e.g. add `wrap/package-single-no-deps`)
 - run `wrap install testauthor/local-pkg-four` in both apps

confirm:
 - both apps converge to the same dependency set after sync

### LOCAL-05: Remove local-dev tracking with uninstall --local-dev

create_package "$TEST_ROOT/local-pkg5" "testauthor/local-pkg-five"
create_app "$TEST_ROOT/app05" "app05"

steps:
 - cd "$TEST_ROOT/local-pkg5" && command: wrap install --local-dev
 - confirm: local-dev tracking exists for the package

command: wrap uninstall --local-dev

confirm:
 - prints `Removed local-dev tracking for ...`
 - running `wrap uninstall --local-dev` again prints `No local-dev tracking found ...` (idempotent)

### ERR-01: Install with --local-dev --from-path with non-existent path

create_app "$TEST_ROOT/err01" "err01"

command: wrap install --local-dev --from-path "$TEST_ROOT/does-not-exist" testauthor/does-not-exist

confirm:
 - command fails
 - dependencies unchanged

### ERR-02: Install multiple packages with --local-dev (should fail)

create_package "$TEST_ROOT/err02-pkg1" "testauthor/err02-pkg1"
create_package "$TEST_ROOT/err02-pkg2" "testauthor/err02-pkg2"
create_app "$TEST_ROOT/err02" "err02"

command: wrap install --local-dev --from-path "$TEST_ROOT/err02-pkg1" testauthor/err02-pkg1 testauthor/err02-pkg2

confirm:
 - command fails (local-dev install supports a single target package)
 - dependencies unchanged

### ERR-03: Use --from-path without --local-dev (should fail)

create_app "$TEST_ROOT/err03" "err03"

command: wrap install --from-path "$TEST_ROOT/some-path" testauthor/some-pkg

confirm:
 - command fails with error indicating `--from-path` requires `--local-dev`
 - dependencies unchanged
