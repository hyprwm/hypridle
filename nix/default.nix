{
  lib,
  stdenv,
  cmake,
  pkg-config,
  hyprlang,
  sdbus-cpp,
  systemd,
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
  };
}
