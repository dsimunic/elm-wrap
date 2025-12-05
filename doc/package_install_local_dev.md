# Installing a package for local development

When developing a package, it is beneficial to be able to install it into an application such that changes
to the package source code are immediately reflected in the application without needing to re-publish the package,
while at the same time ensuring that the application sees the package as a proper dependency with its exposed API.

The command flag `--local-dev` allows for this workflow

    wrap package install --local-dev --from-path /path/to/package author/name

The command behaves as follows:

- Checks source package source path shape  
  This is the same how the `--from-path` flag works already.

- The package will be named as `author/name` as usual.
  If author/name is specified, ensure it matches the one in elm.json. I believe existing `--from-path` machinery already does this.

- Check package version in registry to get the latest published version
  - This is so we can ensure the local dev version is always greater than the published version
  - If the package doesn't exist in registry, we can just use `0.0.0` as the version

- Install symlinks into `ELM_HOME`  
  This is where `--local-dev` differs from `--from-path`: it only creates symlinks to the path in `ELM_HOME` rather than copying the files over.
  Instead of copying the package files to `ELM_HOME/author/name/version/`, it creates a symlink at that path to the source path provided. From the compiler's perspective, the layout in ELM_HOME should look as it expects.

- Once the symlinks are intalled, the process should continue the same as for `--from-path`, after the point it installed the package into `ELM_HOME`. 

- If the installation succeeded, register the app path in dependency-track
    - Create a folder in `ELM_WRAP_REPOSITORY_LOCAL_PATH/local-dev-dependency-track/author/name/version` and add a file containing the path to the elm.json we are adding the package to.

- Exit at this point as usual.

## Defaults

When `--local-dev` flag is used without `--from-path`, the behavior depends on the current directory:

### From within an application directory
If the current directory contains an application (`elm.json` with type `application`), the command will fail since no source path was specified.

### From within a package directory (register-only mode)
If the current directory contains a package (`elm.json` with type `package`), the command operates in **register-only mode**:

- Reads the package name from the local `elm.json`
- Creates a symlink in the cache pointing to this directory  
- Registers the package in the local-dev registry
- Does **NOT** modify any `elm.json` (there's no target application)

This is useful for making a package available to other applications without being in an application directory:

    cd /path/to/my-package
    wrap package install --local-dev
    # Package is now registered in the cache

    # Later, from an application directory:
    cd /path/to/my-app
    wrap package install --local-dev --from-path /path/to/my-package

If `author/name` is specified, it is validated against the package's `elm.json`.

    wrap package install --local-dev author/name
    # Validates that elm.json has the same author/name

## Adding a dependency to the package in development

- Add dependency to the package in development: `wrap package install  <author/name>` 
  - Add dependency as usual
  - Check `local-dev-dependency-track` in the repo
  - Re-install package in all paths you find in that folder in order to update indirect deps