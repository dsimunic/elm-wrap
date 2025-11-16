


## Test dependency, direct, minor upgrade

Desired behavior:

    $ elm-wrap install elm-explorations/test
    I found it in your elm.json file, but in the "test-dependencies" field.
    Should I move it into "dependencies" for more general use? [Y/n]: n
    Okay, I did not change anything!

If the user specifies y, move into 'dependencies' as suggested.

## Test dependency, indirect, minor upgrade

Desired behavior:

    $ elm-wrap install elm/random
    I found it in your elm.json file, but in the "test-dependencies" field.
    Should I move it into "dependencies" for more general use? [Y/n]: n
    Okay, I did not change anything!

If the user specifies y, move into 'dependencies' as suggested.


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

    $ elm-wrap package check
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