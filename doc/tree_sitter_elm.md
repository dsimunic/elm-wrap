We're using the source of the modified tree-sitter-elm parser. 

See Projects/tree-sitter-elm.

Our fixes to the parser are pending on the original github repo, so building/updating the parser with
tree-sitter-cli requires us to pull from this repo, or simply copy tree-sitter's code (it automatically
commpiles into `~/.cache/tree-sitter/lib).