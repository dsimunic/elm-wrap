# Package upgrade policies

`package upgrade <package>` command has different behavior depending on the role of the package being upgraded.

We are assuming a valid elm.json, meaning that all version dependencies are resolved and packages are correctly
placed in direct/indirect relationship.

## Direct dependency, minor upgrade

Package does not change its position in the dependency tree, only its version is updated. Minor versions don't 
change their dependency ranges, so existing indirect dependencies remain the same as before.


    $ wrap package update elm/core

Before:

        "dependencies": {
            "direct": {
                ...
                "elm/core": "1.0.4",


After:

        "dependencies": {
            "direct": {
                ...
                "elm/core": "1.0.5",

Package must transition to 1.0.5. Present the plan before the upgrade unless --yes was specified.


## Indirect dependency, minor upgrade

Package does not change its position in the dependency tree, only its version is updated. 

**happy path:** dependent package's version range allows the upgrade of the dependency -> update only the package without updating its dependent.


    $ wrap package update elm/virtual-dom

Before:

        "dependencies": {
            ...
            },
            "indirect": {
                ...
                "elm/virtual-dom": "1.0.2",


After:

        "dependencies": {
            ...
            },
            "indirect": {
                ...
                "elm/virtual-dom": "1.0.5",

Package must transition to 1.0.5. Present the plan before the upgrade unless --yes was specified.


**unhappy path:** dependent package's version range allows lower version than the latest available, but still higher than current. -> update to the max version allowed.

**unhappy path:** dependent package's version range does not allow minor update. Notify user and exit 1.


## Test dependency, direct, minor upgrade

    wrap package check | grep elm-explorations
    [minor] elm-explorations/test                 1.0.0 -> 1.2.2
    [major] elm-explorations/test                 1.0.0 -> 2.2.0
    


Expected behavior:

    $ wrap package upgrade elm-explorations/test
    --> present plan
    --> upgrade in-place

Before

        "test-dependencies": {
            "direct": {
                "elm-explorations/test": "1.0.0"

After:

        "test-dependencies": {
            "direct": {
                "elm-explorations/test": "1.2.2"


## Test dependency, indirect, minor upgrade

Same behavior as with indirect dependency, minor upgrade.

## Any dependency, no version change

Running upgrade when there are no version changes should not change the position of the dependency.

## Example elm.json


    {
        "type": "application",
        "source-directories": [
            "src"
        ],
        "elm-version": "0.19.1",
        "dependencies": {
            "direct": {
                "Gizra/elm-debouncer": "2.0.0",
                "SiriusStarr/elm-password-strength": "1.0.1",
                "brasilikum/is-password-known": "1.0.0",
                "elm/browser": "1.0.2",
                "elm/core": "1.0.4",
                "elm/html": "1.0.0",
                "elm/http": "1.0.0",
                "elm/svg": "1.0.1"
            },
            "indirect": {
                "TSFoster/elm-sha1": "1.1.0",
                "danfishgold/base64-bytes": "1.0.3",
                "elm/bytes": "1.0.8",
                "elm/json": "1.1.2",
                "elm/parser": "1.1.0",
                "elm/regex": "1.0.0",
                "elm/time": "1.0.0",
                "elm/url": "1.0.0",
                "elm/virtual-dom": "1.0.2",
                "elm-community/dict-extra": "2.4.0",
                "elm-community/list-extra": "8.2.4",
                "rtfeldman/elm-hex": "1.0.0"
            }
        },
        "test-dependencies": {
            "direct": {
                "elm-explorations/test": "1.0.0"
            },
            "indirect": {
                "elm/random": "1.0.0"
            }
        }
    }

Available upgrades for the example:

    $ wrap package check
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