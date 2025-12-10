# Registry support

## Motivation

Elm language development style depends on distribution of source packages through package registries.

In a professional setting, one requires support for multiple levels of distribution and varied policies. For example, a large
multi-developer shop would expect to run its own private, curated collection of packages and enforce specific curation/update/dependency policies. In addition, an organization might want to publish certain open source packages (or even paid packages!) with licensing incompatible with the canonical public Elm package repository. The organization or an individual might seek stronger branding effect through a canonical registry URL for their products.

Furthermore, an individual developer might have the need for local package development and curation of modified packages, and desire the use of package-style distribution instead of vendoring.

Other desiderata might include availability--protection against Internet outages, or even working offline, authentication and authorization, statistics, etc.

Policy-wise, we can imagine an organization internally wanting to distribute packages with ports, or even whole Elm applications! A very desirable property would be to keep all production versions of applications in a package-style repository together with packages it depends on. A work on one package now allows one to instantly determine the impact on all applications where the package is used. Deployment might even easier than pulling and building apps from Git tags!

Clearly there are a number of possible ways one would want to distribute Elm code through the package-style mechanism.

**elm-wrap** caters to this desire, without the burden of forking the canonical compiler.

## Desiderata and constraints

**Policy support** is important: what kind of packages are allowed, how do we enforce it. This needs to be defined per-repository and clearly signaled to the user/machine through both the CLI and web user interface.

**Hierarchical organization** of repositories is an important architectural detail. We want a local repository that feeds into organizational that feeds into the next level and so on. Requests for packages flow from nearest to furthermost, and publishing in the opposite direction.

It is already the case with the canonical repository: local `ELM_HOME` is really a locally cached, pass-through repository. If a package doesn't exist locally, we'll pull it from the canonical repository into the cache for future requests. The inverse is probably not true, though. (Does `publish` leave a copy in `ELM_HOME`?).  

`wrap` further specializes this by suffixing `ELM_HOME` with the Elm compiler
version (for example, `$ELM_HOME/0.19.1/`). Each such per-version cache contains
its own `packages/registry.dat`, and registry providers are expected to expose
only package versions that are compatible with that Elm release. The
client-side solver, especially in major/"any version" upgrade modes, relies on
this contract and does not re-check elm-version compatibility for individual
package versions.

**Authentication/authorization/accounting** is a must for professional work. Proper and clear management, and support on the cli and tooling.

**Package cache format must remain frozen** if we're not to modify the compiler. But that shouldn't be much of an issue as the goal is to distribute the packages to the compiler--no matter the intermediate format, in the end the compiler must see `.elm` source files.

**Persistence/availability** has already bitten a few times, especially in the current "metadata-only" architecture where Git repositories got renamed, or the canonical repository went down. A pro-level repository must store full sources, without further dependencies. This is standard for most package-based systems (whether Debian APT, PHP Composer/Packagist, or what not).

**Trivial deployment** is paramount. Who wants to fiddle with setting things up and running services. A local developer's repository should be a simple drag-and-drop, maybe even just a file-system layout driven by `wrap` directly. For organizational level and wider, some maintenance is expacted and a certain level of control desired.

## Protocol

**Capability discovery** is probably the most basic thing. A tool should discover what a repository can do. Probably the simplest is to just provide a `version` or `level` indicator that a tool can interrogate, and then drive the "standardization" through the tool development. Advanced capability signalling would include a taxonomiy of attributes, or some kind of policy definition. In any case, all this is driven by tool development: it only counts if a client tool can understand it.

## What of the canonical registry?

Basic Elm package registry operates on `https://package.elm-lang.org` and is the canonical "official" registry for packages.

The default Elm package registry is only a metadata registry: it holds package medatata that allows the compiler to perform
dependency resolution, and direct the compiler to a (GitHub-compatible) url where to download the package source.

The Elm compiler has this registry url hard-coded and only knows how to talk to this specific registry type.