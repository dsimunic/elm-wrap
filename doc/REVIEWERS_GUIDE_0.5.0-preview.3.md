# Reviewers Guide

This document guides you through the features of **elm-wrap** version 0.5.0-preview.3, helping you evaluate the feature set in this release.

## Release focus

Release `0.5.0` focuses on developer experience improvements, particularly for package authors. Key highlights include:

- **Local package development**: install local packages from the filesystem into applications and synchronize dependencies during development.

- **Package and application templates**: `application init` and `package init` commands work from built-in templates, allowing quickly starting
    new Elm package projects or Elm and Lamdera projects of all supported kinds.

- **elm.json** management with `install`, `uninstall`, `upgrade`, and `info` commands.



## Feedback sought

The goal of this preview release is to gather feedback on the **usability and functionality of local package development workflows.** Your developer experience
("DX") if you will.

Package development DX is our _beachhead_ feature for developer acceptance of **elm-wrap** in real-world scenarios. That entails the "you don't have to change 
your usual ways of doing things" attitude. Meaning it's easy try it out for this specific workflow without having to buy in into the full upgrade to new repository
format that we can't go back from. One can use `wrap` just to manage local package dev and elm.json, and otherwise forget it exissts. Hence the initial effort to appease the Elm compiler's expectations about package management and dependency resolution. The mantra of this release is "it just works with your usual 
finger memory." For example, we can type `wrap install` or `wrap uninstall`--just like we're used to do with the `elm` command.

Due to the compiler appeasement angle, some rough edges and limitations exist. We could gain a bit more robustness and polish if we could support only 
"run it with `wrap`, always" workflows, but that would be a harder sell for existing Elm developers. At least for now anyway, until the set of useful features
outweighs the pain of change.

The most useful feedback you can provide is **your experience** going through the workflows described below. The ideal case is that you either conclude "Yes, this works as expected, and I can use it in my daily development!" or "No, this doesn't work for me because of REASONS." Both answers are equally useful. Technical issues are inevitable and expected in a preview release, but usability and workflow acceptance is the key goal here. The steps in this guide were verified manually several times, and there are regression tests now, so there's confidence in the basics.

For this review, the feedback channels are:
- "Incremental Elm" Discord, channel #elm-wrap
- Private message on "Incremental Elm" Discord to @Damir.

The hope is we'll turn this `preview.3` quickly, maybe go into `preview.4` if needed, and then release `0.5.0` proper.

The intent for `0.5.0` is to release a solid, production-grade, "daily driver"-quality foundation for local package development workflows and `elm.json` maintenance, and then build on top of it in future releases.

### "Beta testing" vs "Feedback"

You're not "beta testing"--we have specifications and regression tests for that. Some rough edges might show up, but most are known already. 

Your time investment in this review is about trying out the controls and discovering how they feel. Is this how you want your experience to be in the
future when **elm-wrap** reaches v1? If not, now is the right time to discuss anything that feels off or wrong! 

A great example is the change from `preview.2`: we started off with `wrap install author/name 1.0.0` but @jfmengels quickly pointed out that `wrap install author/name 1.0.0 author2/name2 1.0.0` looks unwieldy and un-ergonomic. So now we can run with `wrap install author/name@1.0.0 author2/name2@1.0.0`, which feels much nicer.
Then @wolfadex pointed out his experience when mistyping a package name degraded: `wrap` returned a generic error, where `elm` compiler helpfully tries to propose
some fuzzy matches. Well, `wrap` now does the same!

Version 0.5.0 is the point where we want to fix the interface for package management commands. Nothing is set in stone, but ideally once we is release 0.5.0, the interface will remain unchanged through all iterations until at least 2.x.x series. 

Please, jump in, take `wrap` for a spin and share your experience!

## Changes since `preview.2`

**multiple package specification**: `install`, `uninstall` now accept multiple packages on the command line: `wrap install PACKAGE[@VERSION] [PACKAGE[@VERSION] ...]

**@-separator for versioned package** specification. One can now write `PACKAGE@VERSION` whenever package version specification is allowed.

**aliases for install/uninstall**: `wrap install` and `wrap uninstall` route to corresponding `wrap package COMMAND`. Easier to type and more _in the fingers_. 
This obviously means that `wrap install` is not a pass-through command to the underlying compiler. Though it probably never was.

**`uninstall --local-dev` instead of `install --remove-local-dev`** for symmetry.

**package name suggestions** on mistyped pacakges, identical to Elm compiler's.

## Prerequisites + environment setup

You obviously have to have `wrap` command installed. You can install it by following the instructions in the main [README](README.md).

Local-dev workflow intervenes in data structures and directory layout of the Elm compiler's package cache. The changes are neither invasive nor detrimental to 
the point of functionality or data loss, but may lead to minor annoyances. To avoid interfering with your existing Elm setup, create a parallel Elm package 
cache location for the duration of your review session. 

```bash
mkdir -p /tmp/wrap_review_0.5.0/{elm_home,wrap_home}
ELM_HOME=/tmp/wrap_review_0.5.0/elm_home
WRAP_HOME=/tmp/wrap_review_0.5.0/wrap_home
export ELM_HOME WRAP_HOME
WRAP_FEATURE_CACHE=1 wrap package cache elm/core --prime the package cache; this downloads a fresh copy of the package index
```

Export `ELM_HOME` variable in your shell to point to this new location for the duration of your review session.

If you find all this new environment setup boring, you may just work with your normal `ELM_HOME` setup: `--local-dev` and other registry-index-changing commands
now take care not to upset `elm` compiler's ability to download updates. And, you now have the option to just run `wrap debug registry_v1 reset` and it will
delete the registry index and get you a fresh one just as `elm` would do if you manually deleted that file. This is a handy shortcut so you don't have to think
where your ELM_HOME is.

All flows in this guide assume you have a working Elm compiler (version 0.19.1) or Lamdera (version 0.19.1) installed. No additional installation/initialization is required,
and the actions here should not upset your existing setup or projects. You may stop at any time without side effects. Created projects and package source can be published
on the canonical Elm package registry if desired without any special steps. **elm-wrap** doesn't require any special changes to the source code, nor does it require any new 
files or settings to be tracked in your version control.

Elm / Lamdera compiler binaries are not aware of dependency synchronization for local packages; they work as usual. If you want to continue using dependency synchronization
features, then you'll have to continue using `wrap package install` command.

See [Known issues](#known-issues) for any limitations or issues that may affect your review of this release.

## Features and workflows to review

### Local package development

`local-dev` workflow is useful both when creating a new package never published to the registry before,
as well as developing a new version of an already published package.

Differences between fresh and existing package scenarios are:

- Fresh package requires running `wrap package init PACKAGE`, so you are exercising this feature as well.
- Existing package requires removing any local source directories from `elm.json` of consuming applications.
- The interaction with the package cache is different: the registry index is aware of published package's versions, so installing an existing package will pick up the latest 
published version as the base for local development. Hence you must ensure that the local package's version is higher than the latest published version to avoid confusion.

NOTE: If Elm compiler is not happy with the package's `elm.json` (e.g., invalid name, missing fields, etc.), it will delete it without asking, with the intent to pull it again from registry. 
Make sure your `elm.json` is valid and tracked in version control so you can restore it if needed.

#### New package

In the folder where the package source will reside, run:

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
index as normal. This makes the `package install` command work as expected without any special flags. It also appeases
the stock compiler binary that expects to find the package in the registry at build time.

At this point, `elm install PACKAGE` also works as usual.

#### Existing package

Chances are you have existing packages that you develop locally, likely using the extra path in `source-directories` array
to point to the local source.

Remove the local source directory from `elm.json` and run the following command in the application folder:

```bash
wrap package install --local-dev --from-path ../path-to-local-package  PACKAGE
```

`PACKAGE` must match the name in the package's `elm.json`.

This will install the local package into the application, adding it to `elm.json` dependencies as usual. Additionally, it
will set up dependency synchronization.

This command is equivalent to initializing a new package and adding it to the application, but also works with existing package sources, without having to init them again.

To add the package to another application, simply run the usual `wrap package install PACKAGE` command in that application's folder. 
It is not necessary to pass `--local-dev` or `--from-path` again, as the local package path is stored in the local wrap configuration.


### Info

Running `wrap package info PACKAGE` for the  local package will show the paths to all applications that will be notified 
about the dependency changes.

The same applies to `wrap package application info` command: the report shows the section "Tracking local dev packages:" if it is receiving
dependency updates from any local packages in development.

There's also `wrap info` that will show either an application or package info depending on the current folder. It's an alias for the above commands.

To see all tracked combinations of packages/applications, run `wrap repository local-dev`.


### Dependency synchronization

Go to back to the local package's folder and add a new dependency:

```bash
wrap package install elm/random
```

This will proceed as usual, updating the package's `elm.json` file with new dependency with correct version constraints.

On the bottom of the output of this command, notice:

```
Refreshing 1 dependent application(s)...
Updated indirect dependencies in: /path-to-application/elm.json
``` 

If you check that application's `elm.json`, you will see new indirect dependency has been added as expected. (Of course, if you already had that dependency, nothing changes. Look for packages you never used to see if they got added. I like adding `dillonkearns/elm-pages` as it has many dependencies, so it's guaranteed to add something new.)


Now that you added the dependency to the package, installing it in another application will pick up the new dependency as expected. 

You can verify this by running:

```bash
wrap package install PACKAGE

or 

elm install PACKAGE
```
in another application. The new dependency will show up in the install plan. (Just say `n` to skip installing the local package--this was just to convince yourself it works).

Why does this work? Local application development inserts the local package into the package registry index, and points the cache to the actual package's source directory. This way, the compiler believes the package is installed into the local package cache from the registry as usual, and all dependency resolution works as expected. (That is also why the compiler will delete invalid `elm.json` files--it thinks they are corrupted in the cache and tries to redownload them, but in reality it will find the linked source directory and delete that copy.)

***Removing dependencies*** from the local package synchronizes as well: running `wrap uninstall PACKAGE` on the package directory will update all dependent applications accordingly.

**packages that depend on packages** in local development form a cascade of updates. For example, if package A depends on local package B, and application C depends on package A, then updating dependencies in package B will also update C's indirect dependencies accordingly.

This scenario is NOT supported in this preview release: only updates to directly linked packages update an application's indirect dependencies. 

### Removing local-dev tracking

To stop tracking a package for local development, uninstall it from all applications that depend on it, and then run `wrap repository local-dev` once 
to prune any stale tracking combinations.

Alternatively, you can run this command to stop tracking a specific package for local development:

```bash
wrap registry local-dev remove PACKAGE VERSION
```

You may also remove the package tracking for an app only, by also adding a path to the application folder
as reported by `wrap repository local-dev` command.

```bash
wrap repository local-dev
-- that gives a list of tracked packages and applications
-- then run:
wrap registry local-dev remove PACKAGE VERSION /path/to/application
```
You may also clear all local-dev tracking by running:

```bash
wrap registry local-dev clear --all
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

- `wrap info` shows either application or package info depending on the current folder. It's an alias for `wrap package info` or `wrap package application info`.

## Known issues

- `wrap package install --test PACKAGE` might show confusing error messages in situations with conflicting dependencies.

- The `wrap application info` might continue to report a linked package even after it was removed with `wrap package remove PACKAGE`. 

    Workaround: run `wrap repository local-dev` once to clear stale local-dev tracking information.
