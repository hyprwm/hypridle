# hypridle
Hyprland's idle daemon

## Configuration

Configuration is done via `~/.config/hypr/hypridle.conf` in the standard
hyprland syntax.

```ini
listener {
    timeout = 500                            # in seconds
    on-timeout = notify-send "You are idle!" # command to run when timeout has passed
    on-resume = notify-send "Welcome back!"  # command to run when activity is detected after timeout has fired.
}
```

You can add as many listeners as you please. Omitting `on-timeout` or `on-resume` (or leaving them empty)
will make those events ignored.

## Dependencies
 - wayland
 - wayland-protocols
 - hyprlang >= 0.4.0

## Building & Installation

Building:
```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -S . -B ./build
cmake --build ./build --config Release --target hypridle -j`nproc 2>/dev/null || getconf NPROCESSORS_CONF`
```

Installation:
```sh
sudo cmake --install build
```
