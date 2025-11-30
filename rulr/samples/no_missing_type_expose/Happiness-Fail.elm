module Happiness exposing (happy, toString)

-- Type `Happiness` is private because it's not been exposed

type Happiness
    = Happy

-- Private type `Happiness` used by exposed function `toString`
toString : Happiness -> String
toString happiness =
    "Happy"

-- Private type `Happiness` used by exposed value `happy`
happy : Happiness
happy =
    Happy
