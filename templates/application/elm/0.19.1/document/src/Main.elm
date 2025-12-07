module Main exposing (main)

import Browser
import Html exposing (Html, text)


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


view : Model -> Browser.Document Msg
view model =
    { title = "Document"
    , body = [ text model.message ]
    }


subscriptions : Model -> Sub Msg
subscriptions model =
    Sub.none


main : Program () Model Msg
main =
    Browser.document
        { init = init
        , update = update
        , view = view
        , subscriptions = subscriptions
        }
