{
  lib,
  stdenv,
  cmake,
  pkg-config,
  hyprlang,
  wayland,
  wayland-protocols,
  version ? "git",
}:
stdenv.mkDerivation {
  pname = "hypridle";
  inherit version;
  src = ../.;

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

  buildInputs = [
    hyprlang
    wayland
    wayland-protocols
  ];

  meta = {
    homepage = "https://github.com/hyprwm/hypridle";
    description = "An idle management daemon for Hyprland";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
