# Comprehensive Testing Install/Uninstall Functionality

`install`, `package install`, `package uninstall`, `package upgrade`, `package install --local-dev` commands all 
interact with:
- the file cache
- registry index (for v1 that's ELM_HOME...registry.dat, for v2 that's WRAP_HOME/.../index.dat).
- solver
- elm.json

Commands behave differently depending on whether `elm.json` is application or package type. 

In addition, `package cache` inteeracts with the file cache and the registry index.

All commands receive a package specification that they feed to the solver, receive a plan from the solver and then
proceed with their intended functionality.

NOTE: For release 0.5.0, we are only focused on V1 registry protocol.

## Test corpus

We need a precise and stable test corpus that permits reproducible tests to help us discover regressions quickly.

At a minimum, we must:

- Construct a synthetic registry index(es) with a known state.
- Construct a fixed network of package dependencies we control.

### Synthetic Registry & Cache

We maintain a tool called `indexmaker` that can help us create a registry of imaginary test packages. For v1, we 
must also construct actual package layout in the cache, so we use `mkpkg` tool to do that.

These tools let us construct a unique and reproducible environment for each test. For example, if we need package 
upgrade test, we can just construct a package registry with core elm dependencies (elm/*), a package's version 1 and
version 2, as well as all the dependency tree of these two versions. 

## Tests

A number of tests we have to support is rather large. We must create a massive matrix of all combinations
we want to test, starting with individual commands, followed by combinations. A non-exhaustive list would be:

### package install

- targets: application, package
- cli: single package, multiple packages, targeted version
- exceptions: non-existent package, non-existent version, multiple packages with one or more non-existend packages, multiple packages with one or more with wrong version
- solver: correct dependencies, existing dependencies, moving from indirect to direct, conflicts
- test group: the whole matrix repeated with `--test` switch

### package uninstall

- targets: application, package
- cli: single package 
- exceptions: non-existent package, non-existent version, multiple packages specified
- solver: moving from direct to indirect, oprhaned dependencies
- test group: the whole matrix repeated with `--test` switch

### packgage upgrade

- targets: application, package
- cli: single package, multiple packages
- exceptions: non-existent package, non-existent version, multiple packages with one or more non-existend packages, multiple packages with one or more with wrong version
- solver: moving from direct to indirect, oprhaned dependencies
- test group: the whole matrix repeated with `--test` switch

### package install --local-dev

- targets: application, package
- cli: single package
- exceptions: non-existent path, non-existent version, multiple packages specified, package version already published in registry
- solver: correct dependencies, existing dependencies, moving from indirect to direct, conflicts
- test group: the whole matrix repeated with `--test` switch

### package cache

- cli: single package, multiple packages, from-path, from-url
- exceptions: non-existend package, wrong url, wrong path
- solver: does cache use solver?
- cache: package already present in cache, package not present

## Test procedure

We:
- set up a temporary path,
- define ELM_HOME to correspond to that path,
- stage a list of packages we want in the registry,
- for each package, execute `ELM_HOME=/our/tmp/path mkpkg package`,
- write the registry using `indexmaker package-list TEST_ELM_HOME_PATH/registry.dat`,
- run our tests
- tear down the environment.

This lets us simulate all conditions: conflicts on upgrade, minor/major versions available, etc.