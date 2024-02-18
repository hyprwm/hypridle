self: {
  config,
  pkgs,
  lib,
  ...
}: let
  inherit (builtins) toString;
  inherit (lib.types) bool int listOf package str submodule;
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkOption mkEnableOption;
  inherit (lib.meta) getExe;

  cfg = config.services.hypridle;
in {
  options.services.hypridle = {
    enable = mkEnableOption "Hypridle, Hyprland's idle daemon";

    package = mkOption {
      description = "The hypridle package";
      type = package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.hypridle;
    };

    listeners = mkOption {
      description = "The hypridle listeners";
      type = listOf (submodule {
        options = {
          timeout = mkOption {
            description = "The timeout for the hypridle service, in seconds";
            type = int;
            default = 500;
          };

          onTimeout = mkOption {
            description = "The command to run when the timeout is reached";
            type = str;
            default = "echo 'timeout reached'";
          };

          onResume = mkOption {
            description = "The command to run when the service resumes";
            type = str;
            default = "echo 'service resumed'";
          };
        };
      });
    };

    lockCmd = mkOption {
      description = "The command to run when the service locks";
      type = str;
      default = "echo 'lock!'";
    };

    unlockCmd = mkOption {
      description = "The command to run when the service unlocks";
      type = str;
      default = "echo 'unlock!'";
    };

    afterSleepCmd = mkOption {
      description = "The command to run after the service sleeps";
      type = str;
      default = "echo 'Awake...'";
    };

    beforeSleepCmd = mkOption {
      description = "The command to run before the service sleeps";
      type = str;
      default = "echo 'Zzz...'";
    };

    ignoreDbusInhibit = mkOption {
      description = "Whether to ignore dbus-sent idle-inhibit requests (used by e.g. firefox or steam)";
      type = bool;
      default = false;
    };
  };

  config = mkIf cfg.enable {
    xdg.configFile."hypr/hypridle.conf".text = ''
      general {
        lock_cmd = ${cfg.lockCmd}
        unlock_cmd = ${cfg.unlockCmd}
        before_sleep_cmd = ${cfg.beforeSleepCmd}
        after_sleep_cmd = ${cfg.afterSleepCmd}
        ignore_dbus_inhibit = ${
        if cfg.ignoreDbusInhibit
        then "true"
        else "false"
      }
      }

      ${builtins.concatStringsSep "\n" (map (listener: ''
          listener {
            timeout = ${toString listener.timeout}
            on-timeout = ${listener.onTimeout}
            on-resume = ${listener.onResume}
          }
        '')
        cfg.listeners)}
    '';

    systemd.user.services.hypridle = {
      Unit = {
        Description = "Hypridle";
        After = ["graphical-session.target"];
      };

      Service = {
        ExecStart = "${getExe cfg.package}";
        Restart = "always";
        RestartSec = "10";
      };

      Install.WantedBy = [ "default.target" ];
    };
  };
}
