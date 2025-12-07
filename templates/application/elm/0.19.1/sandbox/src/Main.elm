module Main exposing (main)

import Browser
import Html exposing (Html, text)


type alias Model =
    { message : String }


type Msg
    = Noop


init : Model
init =
    { message = "Hello" }


update : Msg -> Model -> Model
update msg model =
    case msg of
        Noop ->
            model


view : Model -> Html Msg
view model =
    text model.message


main : Program () Model Msg
main =
    Browser.sandbox
        { init = init
        , update = update
        , view = view
        }
