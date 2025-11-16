module Main exposing (Model, Msg(..), init, main, update, view)

import Browser
import Debouncer.Messages as Debouncer exposing (Debouncer, provideInput, settleWhenQuietFor, toDebouncer)
import Html exposing (Html, button, div, fieldset, h3, hr, input, label, li, meter, p, text, ul)
import Html.Attributes exposing (class, disabled, for, id, max, name, type_, value)
import Html.Events exposing (onInput)
import Http
import IsPasswordKnown exposing (IsPasswordKnown(..), hashAndCut, isPasswordKnown, requestPossibleMatches)
import ZxcvbnPlus exposing (Score(..), zxcvbnPlus)



---- MODEL ----


type alias Model =
    { password : String
    , repeat : String
    , passwordDebouncer : Debouncer Msg
    , errorMsg : DialogMessage
    , isPasswordKnown : Maybe IsPasswordKnown
    , passwordScore : Maybe Score
    , dialogMessages : LocalizedMessages
    }


type alias LocalizedMessages =
    { normal : String
    , pwned : String
    , rememberme : String
    , login : String
    , password : String
    , confirmPassword : String
    , resetPassword : String
    }


init : LocalizedMessages -> ( Model, Cmd Msg )
init msgs =
    ( { password = ""
      , repeat = ""
      , passwordDebouncer = Debouncer.manual |> settleWhenQuietFor (Just 500) |> toDebouncer
      , errorMsg = Normal msgs.normal
      , isPasswordKnown = Nothing
      , passwordScore = Nothing
      , dialogMessages = msgs
      }
    , Cmd.none
    )



---- UPDATE ----


type Msg
    = UpdatePwd String
    | UpdateRepeat String
    | PasswordDebounce (Debouncer.Msg Msg)
    | PwnCheck
    | PwnCheckResponse (Result Http.Error String)


update : Msg -> Model -> ( Model, Cmd Msg )
update msg model =
    case msg of
        UpdatePwd s ->
            let
                score =
                    if s == "" then
                        Nothing

                    else
                        Just (zxcvbnPlus [] s |> .score)

                mod_ =
                    { model | password = s, passwordScore = score, errorMsg = Normal model.dialogMessages.normal }
            in
            if s == "" then
                ( mod_, Cmd.none )

            else
                Debouncer.update
                    update
                    updateDebouncer
                    (provideInput PwnCheck)
                    mod_

        UpdateRepeat s ->
            ( { model | repeat = s }, Cmd.none )

        PasswordDebounce subMsg ->
            Debouncer.update
                update
                updateDebouncer
                subMsg
                model

        PwnCheck ->
            ( model, requestPwnedPasswordCheck model.password )

        PwnCheckResponse resp ->
            let
                isKnown =
                    Result.toMaybe (isPasswordKnown model.password resp)

                errMsg =
                    case isKnown of
                        Just (FoundInBreachedDataTimes _) ->
                            Error model.dialogMessages.pwned

                        Just PasswordUnknown ->
                            Normal model.dialogMessages.normal

                        Nothing ->
                            model.errorMsg
            in
            ( { model | isPasswordKnown = isKnown, errorMsg = errMsg }
            , Cmd.none
            )


updateDebouncer : Debouncer.UpdateConfig Msg Model
updateDebouncer =
    { mapMsg = PasswordDebounce
    , getDebouncer = .passwordDebouncer
    , setDebouncer = \debouncer model -> { model | passwordDebouncer = debouncer }
    }


requestPwnedPasswordCheck : String -> Cmd Msg
requestPwnedPasswordCheck pass =
    pass
        |> hashAndCut
        |> requestPossibleMatches
        |> Http.send PwnCheckResponse


scoreToBarLength : Maybe Score -> Int
scoreToBarLength s =
    case s of
        Just sc ->
            case sc of
                TooGuessable ->
                    1

                VeryGuessable ->
                    1

                SomewhatGuessable ->
                    2

                SafelyUnguessable ->
                    3

                VeryUnguessable ->
                    4

        Nothing ->
            0


scoreToSmiley : Maybe Score -> String
scoreToSmiley s =
    case s of
        Just sc ->
            case sc of
                TooGuessable ->
                    "frownie"

                VeryGuessable ->
                    "frownie"

                SomewhatGuessable ->
                    "meh"

                SafelyUnguessable ->
                    "grin"

                VeryUnguessable ->
                    "loveit"

        Nothing ->
            ""



---- VIEW ----


view : Model -> Html Msg
view model =
    fieldset []
        [ h3 []
            [ text "Reset password" ]
        , div [ class "horizontal_rule" ]
            [ hr []
                []
            ]
        , message model.errorMsg
        , ul []
            [ li []
                [ label [ for "password" ]
                    [ text <| model.dialogMessages.password ++ ":" ]
                , div [ class "input_container" ]
                    [ input [ id "password", name "password", type_ "password", class "metered", class <| scoreToSmiley model.passwordScore, value model.password, onInput UpdatePwd ]
                        []
                    , meter
                        [ Html.Attributes.max "4"
                        , id "password-strength-meter"
                        , value (String.fromInt (scoreToBarLength model.passwordScore))
                        ]
                        []
                    ]
                ]
            , li []
                [ label [ for "confirm" ]
                    [ text <| model.dialogMessages.confirmPassword ++ ":" ]
                , div [ class "input_container" ]
                    [ input [ id "confirm", name "confirm", type_ "password", value model.repeat, onInput UpdateRepeat ]
                        []
                    ]
                ]
            , li []
                [ input [ id "rememberH", name "rememberme", type_ "hidden", value "false" ]
                    []
                , label [ for "remember" ]
                    [ input [ id "remember", name "rememberme", type_ "checkbox", value "" ]
                        []
                    , text model.dialogMessages.rememberme
                    ]
                ]
            ]
        , div [ class "form_actions" ]
            [ button [ type_ "submit", value "Login", disabled <| canSubmit model ]
                [ text model.dialogMessages.login ]
            ]
        ]


type DialogMessage
    = Normal String
    | Error String


message : DialogMessage -> Html Msg
message msg =
    case msg of
        Normal s ->
            p []
                [ text s ]

        Error s ->
            p [ class "error" ]
                [ text s ]


canSubmit : Model -> Bool
canSubmit model =
    scoreToBarLength model.passwordScore
        < 2
        || model.password
        == ""
        || model.password
        /= model.repeat
        || (case model.isPasswordKnown of
                Just (FoundInBreachedDataTimes _) ->
                    True

                _ ->
                    False
           )



---- PROGRAM ----


main : Program LocalizedMessages Model Msg
main =
    Browser.element
        { view = view
        , init = init
        , update = update
        , subscriptions = always Sub.none
        }
