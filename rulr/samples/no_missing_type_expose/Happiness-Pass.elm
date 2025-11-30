module Happiness exposing (Happiness, happy, toString)

type Happiness
    = Happy

toString : Happiness -> String
toString happiness =
    "Happy"

happy : Happiness
happy =
    Happy
