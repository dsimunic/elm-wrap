module Main exposing (main)

import Platform exposing (Program)


type alias Model =
    { message : String }


type Msg
    = Noop


init : () -> ( Model, Cmd Msg )
init _ =
    ( { message = "Hello" }, Cmd.none )


update : Msg -> Model -> ( Model, Cmd Msg )
update msg model =
    case msg of
        Noop ->
            ( model, Cmd.none )


subscriptions : Model -> Sub Msg
subscriptions model =
    Sub.none


main : Program () Model Msg
main =
    Platform.worker
        { init = init
        , update = update
        , subscriptions = subscriptions
        }
