# Wrap Manual

## Supported Compilers

Wrap works with the canonical `elm` compiler tool, as well as any of the derived tools like `lamdera` and `wrapc`
that retained the canonical compiler's source layout expectations.

## Wrapper Commands

So-called _wrapper commands_ emulate the compiler's interface. Those are the first commands shown in `--help` output
and they don't bleong to any group:

    wrap --help
    Usage: wrap COMMAND [OPTIONS]

    Commands:

    repl               Open an interactive Elm REPL
    init               Initialize a new Elm project
    reactor            Start the Elm Reactor development server
    make <ELM_FILE>    Compile Elm code to JavaScript or HTML
    install <PACKAGE>  Install packages for your Elm project
    bump               Bump version based on API changes
    diff [VERSION]     Show API differences between versions

    ...

All wrapper commands are "pass-through"--`wrap` interprets the command, takes care of providing any dependencies--based on its
own settings and policies--to the compiler if needed, and then executes the wrapped compiler's program.

For example, `wrap repl` will run the underlying compiler's `repl` command. If you're using the canonical `elm` binary,
then `wrap repl` or `elm repl` will have the same effect. The only difference is that `wrap repl` will use its 
own algorithm and settings to get to any dependencies needed to start the repl, but the outcome will be the same: dependencies
in place where the compiler expects them, and the repl running.

Available commands depend on the wrapped compiler binary. For example, `wrapc` only has a `make` command, so `wrap --help` will
only show that single wrapper command when wrapping `wrapc`. For the canonical compiler, it will show all its commands (except 
`publish`), and similar for `lamdera` it will show all of `lamdera`'s commands. 

If you are developing your own package, or need to check your `elm.json` for any upgrades, the `install` command is useful by itself.
Other commands are mostly intereting if you are using `wrap` repositories.

## Command Groups

`wrap`'s functionality is divided into _command groups_.

### `config` Group

This group has no sub-commands. Typing `wrap config` lists the current `wrap` configuration. Tells you whether you are using the
`wrap` registry format and protocols ("V2") or the canonical `elm` compiler's registry format. Lets you know the path to your 
local repository if in `V2` mode, the compiler binary, compiler version, and the path to the compiler cache (`ELM_HOME`).

Useful for debugging and checking that the setup is what you expect.

### `package` Group

This is the group you'll probably use the most, packages being the primary means of code sharing in Elm world.

Commands in this group support both the usual applicaiton development flow for adding/removing/upgrading dependencies of your
application under development, as well as package development and publishing flow.

Synopsis:

    wrap package COMMAND [OPTIONS]

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


#### **`install`**  

This is the most useful command in this group, and the one you'll likely use most often when developing Elm applications. 

`wrap install <author/package>` does exactly what you'd expect and what you're used to from using the `elm` binary directly:
installs dependencies for your Elm application or a package.

There are a few flags that let you beyond the usual `install` command that might be helpful in certain scenarios:

- `--test`: Install the package as a test dependency. This adds the package under the `test-dependencies` section of your `elm.json`
  instead of the regular `dependencies` section.

- `--major`: Allow upgrading a package to its latest major version. Normally, `wrap install` (like `elm install`) only checks and 
    proposes upgrades to the latest minor version of your dependencies. Use this flag to check what packages have major version 
    updates and install. As usual, it will present you with the plan before implementing any changes.

- `--from-file <FILE_PATH>`: Install a package from a local ZIP file rather than the registry. Useful for testing unpublished packages
  during development or working with private packages without using a local repository:
  
      wrap install --from-file ./my-package.zip author/package

  If the `FILE_PATH` is a directory, it will pick up the files in that directory, as long as they follow the package layout structure.

- `--from-url <URL>`: Install a package from a remote URL pointing to a ZIP file. Handy for installing packages hosted outside 
    the official registry, such as private packages or pre-release versions. 
    
    It's essential for situations where the author a package your application depends on modified a tag, rewrote the repo history 
    or renamed the GitHub repository hosting the package source.
    In that case you may install from any git commit hash URL, or from any url that serves a zip file:
    
        wrap install --from-url https://example.com/my-package.zip author/package

    WARNING: Installing from arbitrary URLs can pose security risks. Ensure you trust the source of the package before using this option.



#### **`upgrade [PACKAGE]`**    

Upgrade packages to their latest available versions. By default, only upgrades to the latest _minor_ version to avoid
breaking changes.

    wrap package upgrade                    # Upgrade all packages to latest minor versions
    wrap package upgrade elm/html           # Upgrade just elm/html

To allow major version upgrades (which may introduce breaking API changes):

    wrap package upgrade --major elm/html   # Upgrade elm/html to latest major version
    wrap package upgrade --major all        # Upgrade everything, including major versions

The `--major-ignore-test` flag allows major upgrades while ignoring conflicts from test dependencies--handy when your
test dependencies haven't caught up with a major release yet.


#### **`remove <PACKAGE>`**     

Remove a package from your Elm project. This also cleans up any indirect dependencies that are no longer needed by other
packages in your project.

    wrap package remove elm/html

Use `-y` or `--yes` to skip the confirmation prompt.

  
#### **`info`**                 

Shows the current `ELM_HOME` directory, registry cache statistics, and package registry connectivity status. 

When run inside an application project directory (that is, a directory with an `elm.json`), it also shows installed packages 
and any available updates.

If you point it to a path with `elm.json`, it will show package information for that package instead:

    wrap package info ./path/to/package

This is super-useful for a quick check of all your projects' dependencies and available updates in a single command.

    find /path/to/projects -name elm.json -print -exec wrap package info {} \;

  
#### **`publish <PATH>`**       

Determine which files should be published from a package. This doesn't actually publish anything--it just shows you what
_would_ be included if you published.

The command uses Datalog rules (`core_package_files`, `publish_files`) to determine the file list. This is the same logic
used by the actual publishing process, so you can verify your package contents before committing to a release.


#### **`docs <PATH>`**          

Generate documentation JSON for an Elm package. This produces the same `docs.json` format that the official package site
uses, which you can use for local documentation tooling or preview purposes.

Point it at a directory containing `elm.json` and `src/`:

    wrap package docs .

Use `--verbose` to see which modules were skipped (e.g., internal modules not exposed in `elm.json`).

#### **`cache [<PACKAGE>]`**    

Download packages to cache without prompting. Useful for pre-populating your cache before going offline, or for CI environments
where you want to ensure all dependencies are available before running builds.


### `repository` Group

Commands for managing local package repositories. This is relevant if you're running in V2 mode and hosting your own packages
or mirroring packages from the official registry.

#### **`new [<root_path>]`**

Create a new repository directory structure at the specified path. If no path is given, uses the current directory. This sets up
the folder layout that `wrap` expects for a local repository.

#### **`list [<root_path>]`**

List all repositories found at the given path. Helpful for discovering what's available in a shared repository location.


### `code` Group

Commands for code analysis and transformation.

#### **`format <FILE>`**

Parse and canonicalize an Elm source file, outputting its AST (Abstract Syntax Tree). This is primarily useful for debugging
or for building tooling that needs to understand Elm source structure.


### `policy` Group

Commands for viewing and managing rulr policy rules. Rulr is `wrap`'s built-in linting and validation system.

#### **`view <RULE>`**

Print the source of a specific rule to stdout. Use this to inspect what a rule does before applying it, or as a starting point
for writing your own rules.

#### **`built-in`**

List all built-in rules that ship with `wrap`. These rules cover common Elm package quality checks like detecting unused
dependencies, redundant files, and missing type exposures.


### `review` Group

Commands for running rulr rules against your Elm code. Think of this as `wrap`'s linting system.

#### **`file <FILE>`**

Analyze a single Elm source file with rulr rules. Outputs any violations found.

#### **`package <PATH>`**

Analyze an entire Elm package directory with rulr rules. This runs all applicable rules against every Elm file in the package
and reports violations. Useful for CI pipelines or pre-publish checks.

Both commands support a `--quiet` (`-q`) flag that suppresses output and exits with code 100 on the first error, or 0 if
everything passes. Handy for scripting.


### `debug` Group

Diagnostic tools for development and troubleshooting.

#### **`include-tree <path>`**

Show the import dependency tree for a file or package. Displays which modules import which other modules, helping you understand
your project's dependency structure and identify potential circular dependencies or overly complex import chains.


## Global Options

These options are available for all commands:

- `-v, --verbose` — Show detailed logging output. Useful for debugging connectivity issues or understanding what `wrap` is doing.
- `-V` — Show the version number.
- `--version` — Show detailed version information, including build details.
- `--sbom, --spdx` — Show the Software Bill of Materials (SBOM) in SPDX format.
- `-h, --help` — Show help message.


## Common Workflows

### Installing a Package

The most common operation. Run from your project directory:

    wrap install elm/http

Or use the wrapper command directly:

    wrap install elm/http

Both do the same thing. If you want to add a package as a test-only dependency:

    wrap install elm/test --test

### Checking for Upgrades

To see what packages have newer versions available:

    wrap package check

This compares your `elm.json` against the registry and shows available updates.

### Upgrading Packages

To upgrade all packages to their latest compatible (minor) versions:

    wrap package upgrade

To upgrade a specific package:

    wrap package upgrade elm/html

To allow major version upgrades (which may have breaking changes):

    wrap package upgrade --major elm/html

Or upgrade everything, including major versions:

    wrap package upgrade --major all

### Installing from Local Sources

Sometimes you need to install a package from a local file or URL rather than the registry:

    wrap install --from-file ./my-package.zip author/package
    wrap install --from-url https://example.com/package.zip author/package

This is useful for testing unreleased packages or working with private packages.

### Reviewing Code Quality

Before publishing a package, run the built-in quality checks:

    wrap review package .

This runs all applicable rules and reports any issues. Use `--quiet` in CI to fail the build on violations:

    wrap review package . --quiet
