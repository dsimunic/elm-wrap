# elm-wrap - elm package management wrapper

This utility wraps Elm compiler and intercepts its package management commands like `install` to augment them with
support for custom package registries and policies.

**elm-wrap** is independent from Elm. It's purpose is to implement flexible package managment for Elm. 
It does not seek to augment or extend Elm the language and its compiler, nor does it interface with Elm compiler's Haskell code in any way. 
It operates its own package management logic, albeit compatible with Elm compiler's expected file layout, and calls the `elm` compiler 
process when all package management is done. The interface to Elm is strictly through process execution.

## Synopsis

    $ export ELM_PACKAGE_REGISTRY_URL=elm-wrap.cloud
    $ wrap install some/package
    $ wrap make examples/src/Main.elm

    All dependencies cached. Running elm make...

    Success! Compiled 1 module.

        Main ───> index.html

    ---

    $ wrap package info

    Package Management Information
    ===============================

    ELM_HOME: ~/.elm/0.19.1

    Registry Cache:
    Location: ~/.elm/0.19.1/packages/registry.dat
    Packages: 2946
    Versions: 16446

    Registry URL: elm-wrap.cloud
    Status: Connected

    Project Information
    -------------------
    Project type: Application
    Installed packages:
    Direct dependencies:      8
    Indirect dependencies:   12
    Test direct:              1
    Test indirect:            1
    Total:                   22

## Commands

### Pass-through

As **elm-wrap** seeks to wrap the compiler, it intercepts all of the Elm compiler's commands:

    wrap repl    
    wrap init    
    wrap reactor 
    wrap make    
    wrap install 
    wrap bump    
    wrap diff    

All of these commands ultimately execute on the Elm compiler(*) after dealing
with any needed package caching.

For example, `wrap make src/Main.elm` will update `registry.dat` (cached version in formation 
from the package registry), read `elm.json` and download from the registry
all packages listed if not already cached, and then call `elm make src/Main.elm` in offline mode. 
(That is, `elm` binary won't make any network requests as it will find all needed packages already in cache).

If you rename `wrap` to `elm` and put it in the path before the elm compiler, it will act as a drop-in
replacement in all scripts and cli usage.

`*` Well, not all: `wrap init` re-implements the init functionality in order to extend it with new
project templating functionality. More on that below.

### Extended commands

Extended comands are organized in sections.

**package** section deals with all package management commands:

    Usage: wrap package SUBCOMMAND [OPTIONS]

    Subcommands:
    install [PACKAGE]              Add a dependency to current elm.json
    upgrade [PACKAGE]              Upgrade packages to latest versions
    remove   PACKAGE               Remove a package from elm.json
    info    [ PATH                 Display package information and upgrades
            | PACKAGE [VERSION]
            ]
    publish PATH                   Show files that would be published from a package
    docs    PATH                   Generate documentation JSON for a package
    cache   [PACKAGE]              Download package to ELM_HOME without adding it to elm.json

    Options:
    -y, --yes            Automatically confirm changes
    -v, --verbose        Show detailed logging output
    -h, --help           Show this help message



**`install`** does full package dependency resolution/download/elm.json updates, without calling elm compiler. It implements
its own package dependency resolution using PubGrub algorithm. It also uses the same resolution strategy ladder as Elm internally,
leading to identical outcomes for install actions.

Most important characteristic of the extended `package install` are `--from-url` and `--from-path` switches: with these, it can install any pckage into the ELM_HOME package tree straight from GitHub or from a local package directory (maybe you are developing a package and want to test without having to push to the canonical public repository, or an author of a package you depend on deleted the version tag or renamed the github repository).

Of note is also the ability to install new major version upgrade, something that the built-in `elm install` cannot do.

    $ wrap package install --major elm/http
    Here is my plan:
    
    Change:
        elm/core    1.0.4 => 1.0.5
        elm/http    1.0.0 => 2.0.0

    Would you like me to update your elm.json accordingly? [Y/n]: 

Synopsis:

    Usage: wrap package install [PACKAGE]

    Install packages for your Elm project.

    Examples:
    wrap install elm/html              # Add elm/html to your project
    wrap install elm/json --test       # Add elm/json as a test dependency
    wrap install --major elm/html      # Upgrade elm/html to next major version
    wrap install --from-file ./pkg.zip elm/html  # Install from local file
    wrap install --from-url <url> elm/html       # Install from URL

    Options:
    --test                             # Install as test dependency
    --major <package>                  # Allow major version upgrade for package
    --from-file <path> <package>       # Install from local file/directory
    --from-url <url> <package>         # Install from URL (skip SHA check)
    --pin                              # Create PIN file with package version
    -v, --verbose                      # Show progress reports (registry, connectivity)
    -q, --quiet                        # Suppress progress reports
    -y, --yes                          # Automatically confirm changes
    --help                             # Show this help


**`remove`** removes a package from `elm.json`, together with any indirect dependencies that would become orphaned.

**`upgrade`** does minor and major version upgrades of packages in a project's elm.json:

    $ wrap package upgrade
    Here is my plan:
    
    Change:
        SiriusStarr/elm-password-strength    1.0.1 => 1.0.2
        danfishgold/base64-bytes             1.0.3 => 1.1.0
        elm/core                             1.0.4 => 1.0.5
        elm/html                             1.0.0 => 1.0.1
        elm/json                             1.1.2 => 1.1.4
        elm/virtual-dom                      1.0.2 => 1.0.5
        elm-community/list-extra             8.2.4 => 8.7.0
        elm-explorations/test                1.0.0 => 1.2.2
    

    Would you like me to update your elm.json accordingly? [Y/n]: 

**`info`** sub-command that can check for available upgrades, both minor and major:

Synopsis:

    Usage: wrap package info [PATH | <author/package> [VERSION]]

    Display package management information.

    Shows:
    - Current ELM_HOME directory
    - Registry cache statistics
    - Package registry connectivity
    - Installed packages (if run in a project directory)
    - Available updates (if run in a project directory)

    Version resolution (for package lookup):
    - If package is in elm.json: uses that version
    - If not in elm.json and no VERSION specified: uses latest from registry
    - If VERSION specified: uses that specific version

    Examples:
    wrap package info                  # Show general package info
    wrap package info ./path/to/dir    # Show info for elm.json at path
    wrap package info elm/core         # Show info for elm/core package
    wrap package info elm/http 2.0.0   # Show info for elm/http 2.0.0

    Note: Package name format (author/package) takes priority over paths.
        Use './package/author' to treat as a path instead.

    Options:
    --help                             # Show this help

Example output:

    $ wrap package info
    Available upgrades:

    [minor] SiriusStarr/elm-password-strength     1.0.1 -> 1.0.2
    [major] TSFoster/elm-sha1                     1.1.0 -> 2.1.1
    [minor] danfishgold/base64-bytes              1.0.3 -> 1.1.0
    [minor] elm-community/list-extra              8.2.4 -> 8.7.0
    [minor] elm-explorations/test                 1.0.0 -> 1.2.2
    [major] elm-explorations/test                 1.0.0 -> 2.2.0
    [minor] elm/core                              1.0.4 -> 1.0.5
    [minor] elm/html                              1.0.0 -> 1.0.1
    [major] elm/http                              1.0.0 -> 2.0.0
    [minor] elm/json                              1.1.2 -> 1.1.4
    [minor] elm/virtual-dom                       1.0.2 -> 1.0.5


Conveniently, `info` also takes the path to elm.json, so one can check all projects in a directory tree with:

    find /path/to/projects -name elm.json -print -exec wrap package info {} \;

**`deps <package>`** lists the dependencies of a package.

## Configuration

`wrap` takes configuration options through environment variables and command flags.

**`ELM_HOME`** is the same as `elm` compiler's home variable. It will pass it on to the compiler invocations as well.
Defaults to `~/.elm/0.19.1`.

**`ELM_PACKAGE_REGISTRY_URL`** configures the package registry to use in all package-download-causing commands. Behind the
scenes `wrap` relies on cUrl library, so it will respect all of cUrl's environment variables as well.
Defaults to `https://package.elm-lang.org.

**`WRAP_ELM_COMPILER_PATH`** specifies the `elm` binary to call from commands like `make` and others. If not specified,
`wrap` will find the first `elm` binary in it's `PATH` environment variable.

**`WRAP_ALLOW_ELM_ONLINE`** disables `elm` compiler's "offline mode." Specify any value, the program tests for presence.
The default is to insert an invalid proxy variable into `elm` binary's environment  (`https_proxy=http://1`) forcing the compiler
to go into "offline mode" as it cannot connect to the said address. The only reason `elm` compiler binary needs to go online
is to contact the package registry that is hard-coded to `package.elm-lang.org`. Obviously not very useful if our goal is to 
use our own package registry.

**`WRAP_OFFLINE_MODE`** forces **elm-wrap** itself to run without touching the network. Set it to `1` to skip registry connectivity
checks and registry updates entirely. Commands then rely solely on any cached `registry.dat`/`index.dat` that already exists in
`ELM_HOME`. If no cache is present, commands will fail immediately with a hint to unset the variable or run online once to
populate the cache.


## Status

`wrap` is functional as described above except for `wrap diff`, `bump` and `publish` commands. `elm`'s handling of `diff` command insists on access to the online registry despite all available information being in the package cache.

Package development-related commands (`bump` and `publish`) were not tested. Obviously, `publish` won't work as is.

Re-implementation of both commands is planned shortly.

The code builds on macOS and linux, and is mildly manually tested on a sample of author's elm projects. A more thorough testing must encompass much wider use.

The core motivation for writing this tool is to install lydell's forks of elm/core, elm/virtual-dom and friends. This works now, but it makes sense to continue to complete the tool. Most work should happen on separate package registry implementations, though. See [an initial writeup on the subject](doc/registry_support.md).

Pinning is still not in place: the idea being that we don't want to forget and upgrade an override package we installed, and then wonder what went wrong. There's more to say about pinning, including permanently looking for updates on the overriden location. But that's not yet here. Hopefully soon.

As expected, tests are behind. `make check` just runs a couple of tests. The real work will be to test PubGrub algorithm thoroughly.

## Installation


### Homebrew

```bash
brew tap dsimunic/elm-wrap
brew install elm-wrap
```

Note the binary command is called `wrap`.

You can confirm you have the latest version with:

```bash
wrap --version
```

Later updates:

```bash
brew upgrade elm-wrap
```

### Download Pre-built Binaries


**macOS (Apple Silicon / ARM64)**: [elm-wrap-macos-arm64](https://github.com/OWNER/REPO/releases/latest/download/elm-wrap-macos-arm64)
```
curl -L https://github.com/OWNER/REPO/releases/latest/download/elm-wrap-macos-arm64 -o wrap
chmod +x wrap
xattr -d com.apple.quarantine wrap
mv wrap ~/.local/bin 
or
sudo mv wrap /usr/local/bin/
```

### Linux

Sadly, for now you'll need to make your own. Get the source from the release page:

**Build from source**: [Source code (tar.gz)](https://github.com/dsimunic/elm-wrap/archive/refs/tags/v0.5.0-preview.1.tar.gz)

Then unpack and build with `make all install` or `make all install-user` as appropriate.

You'll probably need to install a few dependencies first.

### Docker development environment.

Follow the advice here on installing Docker,
[install docker](https://docs.docker.com/engine/install/debian/)
as well as setting up non-root users to be able to use it,
[non-root users](https://docs.docker.com/engine/install/linux-postinstall/#manage-docker-as-a-non-root-user).

To confirm it is working correctly run these commands (`newgrp docker` only
needs to be run if you are not logging out and back in again):

    newgrp docker
    docker run hello-world

To build the docker image:

    docker build -t wrap-dev:bookworm .

To run an interactive session as your user inside the docker container:

    docker run -it --rm -v "$PWD":/work wrap-dev:bookworm bash

Build the code with make:

    make

## Prior art and similar utilities

**elm-wrap** is independently thought of and created. 

Obviously, it depends on the awesome `elm` compiler and would serve no purpose without it. This program uses a few prompts from the elm compiler under [BSD 3-clause license](doc/licenses/elm-compiler-LICENSE).

**elm-wrap** implements PubGrub algorithtm for version dependency resolution based on [PubGrub description](https://github.com/dart-lang/pub/blob/master/doc/solver.md). The text is also copied to this repository under [BSD 3-clause license](doc/licenses/pubgrub-solver-spec.md-LICENSE). 

[elm-json](https://github.com/zwilias/elm-json) deals with editing `elm.json` configuration. The project has a focus on editing `elm.json` in sophisticated ways. While **elm-wrap** provides similar functionality, its main objective is package management through custom registries and policies.
