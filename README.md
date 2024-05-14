# hypridle
Hyprland's idle daemon

## Features
 - based on the `ext-idle-notify-v1` wayland protocol
 - support for dbus' loginctl commands (lock / unlock / before-sleep)
 - support for dbus' inhibit (used by e.g. firefox / steam)

## Configuration

Configuration is done via `~/.config/hypr/hypridle.conf` in the standard
hyprland syntax.

```ini
general {
    lock_cmd = notify-send "lock!"          # dbus/sysd lock command (loginctl lock-session)
    unlock_cmd = notify-send "unlock!"      # same as above, but unlock
    before_sleep_cmd = notify-send "Zzz"    # command ran before sleep
    after_sleep_cmd = notify-send "Awake!"  # command ran after sleep
    ignore_dbus_inhibit = false             # whether to ignore dbus-sent idle-inhibit requests (used by e.g. firefox or steam)
    ignore_systemd_inhibit = false          # whether to ignore systemd-inhibit --what=idle inhibitors
}

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
 - sdbus-c++

## Building & Installation

### Building:
```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -S . -B ./build
cmake --build ./build --config Release --target hypridle -j`nproc 2>/dev/null || getconf NPROCESSORS_CONF`
```

### Installation:
```sh
sudo cmake --install build
```

### Usage:

Hypridle should ideally be launched after logging in. This can be done by your compositor or by systemd.
For example, for Hyprland, use the following in your `hyprland.conf`.
```hyprlang
exec-once = hypridle
```
If, instead, you want to have systemd do this for you, you'll just need to enable the service using
```sh
systemctl --user enable --now hypridle.service
```

## Flags

```
-c <config_path>, --config <config_path>: specify a config path, by default
                                          set to ${XDG_CONFIG_HOME}/hypr/hypridle.conf
-q, --quiet
-v, --verbose
```
