{
  description = "Hyprland's idle daemon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";

    hyprlang = {
      url = "github:hyprwm/hyprlang";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
      inputs.hyprutils.follows = "hyprutils";
    };

    hyprutils = {
      url = "github:hyprwm/hyprutils";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
    };

    hyprland-protocols = {
      url = "github:hyprwm/hyprland-protocols";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
    };

    hyprwayland-scanner = {
      url = "github:hyprwm/hyprwayland-scanner";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
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
      hypridle = builtins.throw "hypridle: the flake HM module has been removed. Use the module from Home Manager upstream.";
    };

    checks = eachSystem (system: self.packages.${system});

    formatter = eachSystem (system: pkgsFor.${system}.alejandra);
  };
}
