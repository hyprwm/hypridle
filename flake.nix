{
  description = "Hyprland's idle daemon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    hyprlang = {
      url = "github:hyprwm/hyprlang";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = inputs: let
    inherit (inputs.nixpkgs) lib;
    genSystems = lib.genAttrs [
      # Add more systems if they are supported
      "x86_64-linux"
      "aarch64-linux"
    ];
    pkgsFor = genSystems (system:
      import inputs.nixpkgs {
        overlays = [inputs.self.overlays.default];
        inherit system;
      });
  in {
    overlays = import ./nix/overlays.nix {inherit inputs lib;};

    packages = genSystems (system: {
      inherit (pkgsFor.${system}) hypridle;
      default = inputs.self.packages.${system}.hypridle;
    });

    checks = genSystems (system: inputs.self.packages.${system});

    formatter = genSystems (system: pkgsFor.${system}.alejandra);
  };
}
