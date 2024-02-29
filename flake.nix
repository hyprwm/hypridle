{
  description = "Hyprland's idle daemon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";

    hyprlang = {
      url = "github:hyprwm/hyprlang";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = {
    self,
    nixpkgs,
    systems,
    ...
  } @ inputs: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);

    pkgsFor = eachSystem (system:
      import nixpkgs {
        localSystem.system = system;
        overlays = with self.overlays; [default];
      });
  in {
    overlays = import ./nix/overlays.nix {inherit inputs lib;};

    packages = eachSystem (system: {
      default = self.packages.${system}.hypridle;
      inherit (pkgsFor.${system}) hypridle;
    });

    homeManagerModules = {
      default = self.homeManagerModules.hypridle;
      hypridle = import ./nix/hm-module.nix self;
    };

    checks = eachSystem (system: self.packages.${system});

    formatter = eachSystem (system: pkgsFor.${system}.alejandra);
  };
}
