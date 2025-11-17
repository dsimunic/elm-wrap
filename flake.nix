{
  description = "elm-wrap - elm package management wrapper";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/release-25.05";

  outputs =
    { self, ... }@inputs:

    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forEachSupportedSystem =
        f:
        inputs.nixpkgs.lib.genAttrs supportedSystems (
          system:
          f {
            pkgs = import inputs.nixpkgs { inherit system; };
          }
        );
    in
    {
      packages = forEachSupportedSystem (
        { pkgs }:
        {
          default = pkgs.stdenv.mkDerivation {
            name = "elm-wrap";
            version = builtins.readFile ./VERSION;
            src = ./.;

            nativeBuildInputs = with pkgs; [
              hostname
            ];
            buildInputs = with pkgs; [
              curl
            ];

            buildPhase = ''
              make
            '';

            installPhase = ''
              PREFIX=$out make install
            '';
          };
        }
      );
    };
}
