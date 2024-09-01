{
  lib,
  stdenv,
  cmake,
  pkg-config,
  hyprlang,
  hyprutils,
  sdbus-cpp,
  systemd,
  wayland,
  wayland-protocols,
  wayland-scanner,
  version ? "git",
}:
stdenv.mkDerivation {
  pname = "hypridle";
  inherit version;
  src = ../.;

  nativeBuildInputs = [
    cmake
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    hyprlang
    hyprutils
    sdbus-cpp
    systemd
    wayland
    wayland-protocols
  ];

  meta = {
    homepage = "https://github.com/hyprwm/hypridle";
    description = "An idle management daemon for Hyprland";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "hypridle";
  };
}
