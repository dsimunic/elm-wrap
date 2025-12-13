# Reviewers Guide

This document guides you through the features of **elm-wrap** version 0.5.0-preview.3, helping you evaluate the feature set in this release.

## Release focus

Release `0.5.0` focuses on developer experience improvements, particularly for package authors. Key highlights include:

- **Local package development**: install local packages from the filesystem into applications and synchronize dependencies during development.

- **Package and application templates**: `application init` and `package init` commands work from built-in templates, allowing you to quickly start new Elm package projects or Elm and Lamdera projects of all supported kinds.

- **elm.json** management with `install`, `uninstall`, `upgrade`, and `info` commands.



## Feedback sought

The goal of this preview release is to gather feedback on the **usability and functionality of local package development workflows.** Your developer experience
("DX") if you will.

Package development DX is our _beachhead_ feature for developer acceptance of **elm-wrap** in real-world scenarios. That entails the "you don't have to change 
your usual ways of doing things" attitude. Meaning it's easy to try it out for this specific workflow without having to buy into the full upgrade to the new repository format that we can't go back from. You can use `wrap` just to manage local package development and elm.json, and otherwise forget it exists. Hence the initial effort to satisfy the Elm compiler's expectations about package management and dependency resolution. The mantra of this release is "it just works with your usual muscle memory." For example, you can type `wrap install` or `wrap uninstall` — just like you're used to with the `elm` command.

Due to the compatibility-focused approach with the Elm compiler, some rough edges and limitations exist. We could gain more robustness and polish if we only supported
"run it with `wrap`, always" workflows, but that is a harder sell for for now, until the set of useful features greatly outweighs the pain of change.

The most useful feedback you can provide is **your experience** going through the workflows described below. The ideal case is that you either conclude "Yes, this works as expected, and I can use it in my daily development!" or "No, this doesn't work for me because of reasons." Both answers are useful. Technical issues are inevitable and expected in a preview release, but usability and workflow acceptance is the key goal here. The steps in this guide were verified manually several times, and there are regression tests now, so there's confidence in the basics.

For this review, the feedback channels are:
- "Incremental Elm" Discord, channel #elm-wrap
- Private message on "Incremental Elm" Discord to @Damir.

We hope to iterate on `preview.3` quickly, possibly release `preview.4` if needed, and then release `0.5.0`.

The intent for `0.5.0` is to release a solid, production-grade, **daily driver**-quality foundation for local package development workflows and `elm.json` maintenance, and then build on top of it in future releases.

### "Beta testing" vs "Feedback"

You're not "beta testing"--we have specifications and regression tests for that. Some rough edges might show up, but most are known already. 

Your time investment in this review is about trying out the controls and discovering how they feel. Is this how you want your experience to be in the
future when **elm-wrap** reaches v1? If not, now is the right time to discuss anything that feels off or wrong! 

Great examples are the changes from `preview.2`: 
 - @wolfadex pointed out his experience when mistyping a package name degraded: `wrap` returned a generic error, where `elm` compiler helpfully tries to propose
some fuzzy matches. Well, `wrap` now does the same!
- We started off with `wrap install author/name 1.0.0` but @jfmengels quickly pointed out that `wrap install author/name 1.0.0 author2/name2 1.0.0` looks unwieldy and unintuitive. Now you can run `wrap install author/name@1.0.0 author2/name2@1.0.0`, which feels much nicer.


Version 0.5.0 is the point where we want to fix the interface for package management commands. Nothing will be set in stone, but ideally once we release 0.5.0, the package management interface will remain unchanged through future releases up to at least the 2.x.x series. 

Please, jump in, take `wrap` for a spin and share your experience!

## Changes since `preview.2`

**multiple package specification**: `install`, `uninstall` now accept multiple packages on the command line: `wrap install PACKAGE[@VERSION] [PACKAGE[@VERSION] ...]`

**@-separator for versioned package** specification. One can now write `PACKAGE@VERSION` whenever package version specification is allowed.

**aliases for install/uninstall**: `wrap install` and `wrap uninstall` route to corresponding `wrap package COMMAND`. Easier to type and more _in the fingers_. 
Consequently, `wrap install` is not a pass-through command to the underlying compiler.

**`uninstall --local-dev` instead of `install --remove-local-dev`** for symmetry.

**package name suggestions** for mistyped packages, identical to the Elm compiler's suggestions.

**`cache` command group** is now visible, so you don't need to specify `WRAP_FEATURE_CACHE=1` feature flag.

## Prerequisites + environment setup

You must have the `wrap` command installed. You can install it by following the instructions in the main [README](README.md).

Local-dev workflow modifies the Elm compiler's package cache directory layout and related data structures. The changes are neither invasive nor likely to cause functionality or data loss, but may lead to minor annoyances. To avoid interfering with your existing Elm setup, create a parallel Elm package 
cache location for the duration of your review session. 

```bash
mkdir -p /tmp/wrap_review_0.5.0/{elm_home,wrap_home}
ELM_HOME=/tmp/wrap_review_0.5.0/elm_home
WRAP_HOME=/tmp/wrap_review_0.5.0/wrap_home
export ELM_HOME WRAP_HOME
wrap package cache elm/core --prime the package cache; this downloads a fresh copy of the package index
```

Export `ELM_HOME` variable in your shell to point to this new location for the duration of your review session.

If you prefer, you can continue using your normal `ELM_HOME` setup: `--local-dev` and other registry-index-changing commands now take care not to upset the Elm compiler's ability to download updates. You also have the option to run `wrap debug registry_v1 reset`, which will delete the registry index and get you a fresh one just as `elm` would do if you manually deleted that file. This is a handy shortcut so you don't have to worry about where your ELM_HOME is.

All flows in this guide assume you have a working Elm compiler (version 0.19.1) or Lamdera (version 0.19.1) installed. No additional installation/initialization is required,
and the actions here should not upset your existing setup or projects. You may stop at any time with no side effects. Created projects and package source can be published on the canonical Elm package registry without any special steps. **elm-wrap** doesn't require any special changes to the source code, nor does it require any new files or settings to be tracked in your version control.

Elm and Lamdera compiler binaries are not aware of dependency synchronization for local packages; they continue to operate as usual. If you want to continue using dependency synchronization
features, then you'll have to continue using the `wrap package install` command.

See [Known issues](#known-issues) for any limitations or issues that may affect your review of this release.

## Features and workflows to review

### Short and long commands, aliases

There's a symmetry between `wrap package COMMAND` and `wrap application COMMAND` support where it makes sense. The review guide tries to use the long-form versions for clarity, but sometimes it's obvious the long-form will rarely be used, so you may prefer the short version. Shortcuts must select whether to run package or application commands; the choices are explained below.

Because we aim for ergonomic commands while preserving consistency, here are a few command aliases to know:

- `wrap init` is the same as `wrap application init`.  
   The default template is `Browser.application`. Because it's a shortcut, you can still use all the `application` command options and flags. For example: `wrap init worker`.  
   To initialize a package, use the long form: `wrap package init PACKAGE`.
- `wrap install` is the same as `wrap package install`.
- `wrap uninstall` is the same as `wrap package uninstall`.
- `wrap info` is the same as both `wrap package info` and `wrap application info`  
   The command reads the package type from elm.json and decides what information to show.

There's also an `app` alias for `application`, intended as a convenience for experienced users; it will not be documented in the main manual but is noted here for reviewers.

### Local package development

**local-dev** workflow is useful both when creating a new package never published to the registry before,
as well as developing a new version of an already published package.

Differences between fresh and existing package scenarios are:

- Fresh package requires running `wrap package init PACKAGE`, so you are exercising this feature as well.
- Existing package requires removing any local source directories from `elm.json` of consuming applications.
- The interaction with the package cache is different: the registry index is aware of published package's versions, so installing an existing package will pick up the latest 
published version as the base for local development. Hence you must ensure that the local package's version is higher than the latest published version to avoid confusion.

NOTE: If the Elm compiler is not happy with the package's `elm.json` (e.g., invalid name, missing fields, etc.), it may delete that cached copy and attempt to redownload it from the registry. 
Make sure your `elm.json` is valid and tracked in version control so you can restore it if needed.

#### New package

`cd` to the folder where the package source will reside, and run:

```bash
wrap package init PACKAGE
```

As expected, this will create an `elm.json` file and an empty `src` folder. `elm.json`
will have the `type` set to `package` and use the name you provided on the command line as the argument.

The package starts at version `1.0.0` and only has `elm/core` as a dependency.

The license is hard-coded to `BSD-3-Clause` for now.

Install the package in an application as any other published package:

```bash
cd ../example-app
wrap package install PACKAGE
```

This will install the local package into the application, adding it to `elm.json` dependencies as usual.

This works because `wrap` starts tracking the local package path on `package init`, and also adds it to Elm's package registry
index as normal. This makes the `package install` command work as expected without any special flags. It also satisfies the Elm compiler, which expects to find the package in the registry at build time.

At this point, `elm install PACKAGE` will also work as usual.

#### Existing package

Chances are you have existing packages that you develop locally, likely using the extra path in `source-directories` array
to point to the local source.

Remove the local source directory from `elm.json` and run the following command in the application folder:

```bash
wrap package install --local-dev --from-path ../path-to-local-package  PACKAGE
```

`PACKAGE` must match the name in the package's `elm.json`.

This will install the local package into the application, adding it to `elm.json` dependencies as usual and setting up dependency synchronization.

This command is equivalent to initializing a new package and adding it to the application, but also works with existing package sources, without having to initialize them again.

To add the package to another application, simply run the usual `wrap package install PACKAGE` command in that application's folder. 
It is not necessary to pass `--local-dev` or `--from-path` again; the local package path is stored in the local `wrap` configuration.

If, for whatever reason, you are starting a clean package project at a version higher than 1.0.0, there's the usual PACKAGE@VERSION:


```bash
wrap package init my/package@2.0.1

Here is my plan:
  
  Create new elm.json for the package:
    my/package    2.0.1
  
  Source: /private/tmp/elm-wrap-test-shell-84823/workspace
  
  Also, I will register the package for local development. To prevent that,
  run this command again and specify --no-local-dev flag.


To use this package in an application, run from the application directory:
    wrap package install my/package
  
---
wrap info
...
Type: Package
Name: my/package
Version: 2.0.1
...
```


### Info

Running `wrap info PACKAGE` for the local package will show the paths to all applications that will be notified about the dependency changes.

The same applies to `wrap application info` command: the report shows the section "Tracking local dev packages:" if it is receiving
dependency updates from any local packages in development.

There's also `wrap info`, which shows application or package info depending on the current folder. It's an alias for the above commands.

To see all tracked combinations of packages/applications, run `wrap repository local-dev`.

The `wrap info PACKAGE` variant looks in the registry and can get you the info for any package, not just the local package. You may wonder what dependencies does `elm/html` have:

```bash
wrap info elm/html

Package: elm/html
Version: 1.0.1
Total versions: 2


Package: elm/html 1.0.1
========================================

Dependencies (3):
  elm/core           1.0.0 <= v < 2.0.0
  elm/json           1.0.0 <= v < 2.0.0
  elm/virtual-dom    1.0.0 <= v < 2.0.0
```

Like elsewhere, you may specify the version as well:

```bash
wrap info elm/html@1.0.0

Package: elm/html
Version: 1.0.0
Latest version: 1.0.1
Total versions: 2


Package: elm/html 1.0.0
========================================

Dependencies (3):
  elm/core           1.0.0 <= v < 2.0.0
  elm/json           1.0.0 <= v < 2.0.0
  elm/virtual-dom    1.0.0 <= v < 2.0.0
```

Not a very exciting example, but still, you know what's possible.


### Dependency synchronization

Go back to the local package's folder and add a new dependency:

```bash
wrap install elm/random
```

This will proceed as usual, updating the package's `elm.json` file with a new dependency and the correct version constraints.

At the bottom of the command output, notice:

```
Refreshing 1 dependent application(s)...
Updated indirect dependencies in: /path-to-application/elm.json
``` 

If you check that application's `elm.json`, you will see the new indirect dependency has been added as expected. (Of course, if you already had that dependency, nothing changes. Look for dependencies you don't already have to see if they were added. I like adding `dillonkearns/elm-pages` as it has many dependencies, so it's guaranteed to add something new.)


Now that you added the dependency to the package, installing it in another application will pick up the new dependency as expected. 

You can verify this by running:

```bash
wrap install PACKAGE
```
in another application. The new dependency will show up in the install plan. (Answer `n` to skip installing the local package; this was just to confirm it works.)

Why does this work? Local application development inserts the local package into the package registry index, and points the cache to the actual package's source directory. This way, the compiler believes the package is installed into the local package cache from the registry as usual, and all dependency resolution works as expected. (That is also why the compiler will delete invalid `elm.json` files--it thinks they are corrupted in the cache and tries to redownload them, but in reality it will find the linked source directory and delete that copy).

***Removing dependencies*** from the local package synchronizes as well: running `wrap uninstall PACKAGE` on the package directory will update all dependent applications accordingly.

**packages that depend on packages** in local development form a cascade of updates. For example, if package A depends on local package B, and application C depends on package A, then updating dependencies in package B will also update C's indirect dependencies accordingly. **This scenario is not supported in this preview release.** Only updates to directly linked packages update an application's indirect dependencies.

### Removing local-dev tracking

To stop tracking a package for local development, uninstall it from all applications that depend on it, and then run `wrap repository local-dev` once 
to prune any stale tracking combinations.

Alternatively, you can run this command to stop tracking a specific package for local development:

```bash
wrap repository local-dev remove PACKAGE VERSION
```

You may also remove the package tracking for an app only by also adding a path to the application folder
as reported by `wrap repository local-dev` command.

```bash
wrap repository local-dev
-- that gives a list of tracked packages and applications
-- then run:
wrap repository local-dev remove PACKAGE VERSION /path/to/application
```
You may also clear all local-dev tracking by running:

```bash
wrap repository local-dev clear --all
```

### Initializing projects from templates

`wrap application init` and `wrap package init` commands support creating new projects from built-in templates. 

To see the list of available templates, run:

```bash
wrap application list-templates
```

Depending on the compiler in use, you will see templates for Elm or Lamdera projects.

To create a new `Browser.document` project from a template, run:

```bash
wrap application init document
```

Similar for other types. The default is `application` template, so `wrap application init` works as before, as does `wrap init` that is an alias for it.

A new package project can be created from a template as well:

```bash
wrap package init PACKAGE
```

This correctly generates a package project and inserts the author/package-name into the `elm.json` file.

There is currently only one package template, so no need to specify it.


### Handy commands

- `wrap config` shows the current configuration, including `ELM_HOME` and `WRAP_HOME` locations.

- `wrap info` shows either application or package info depending on the current folder. It's an alias for `wrap package info` or `wrap application info`.

## Known issues

- `wrap package install --test PACKAGE` might show confusing error messages in situations with conflicting dependencies.

- The `wrap application info` might continue to report a linked package even after it was removed with `wrap package uninstall PACKAGE`. 

    Workaround: run `wrap repository local-dev` once to clear stale local-dev tracking information.

- This is the stauts of all commands that accept a `PACKAGE` argument with respect to the new PACKAGE[@VERSION] syntax.

  - `wrap package install` accepts `PACKAGE`, `PACKAGE@VERSION`, and `PACKAGE VERSION`.
  - `wrap package cache` accepts `PACKAGE`, `PACKAGE@VERSION`, and `PACKAGE VERSION`.
  - `wrap package cache check` does not accept a version argument.
  - `wrap package init` accepts `PACKAGE`, `PACKAGE@VERSION`, and `PACKAGE VERSION`.
  - `wrap package info` accepts `PACKAGE`, `PACKAGE@VERSION`, and `PACKAGE VERSION`.
  - `wrap package uninstall` accepts `PACKAGE [PACKAGE...]` and does not accept a version specification.
  - `wrap package upgrade` accepts only `PACKAGE` or `all`.
  - `wrap debug install-plan` does not accept a version specification.
  - `wrap repository local-dev clear PACKAGE VERSION [...]` accepts a `VERSION` argument but treats it as an opaque string without semver validation. This is due to the way `clear` works on matching stored strings.

- Note that package name validation differs from Elm: Elm doesn't accept package name that starts with a number, while `wrap` presently does:
  `elm install aaaa/0000` will fail in elm but not in `wrap`.