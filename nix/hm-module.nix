self: {
  config,
  pkgs,
  lib,
  ...
}: let
  inherit (builtins) toString;
  inherit (lib.types) str int package;
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

    timeout = {
      description = "The timeout for the hypridle service, in seconds";
      type = int;
      default = 500;
    };

    onTimeout = {
      description = "The command to run when the timeout is reached";
      type = str;
      default = "echo 'timeout reached'";
    };

    onResume = {
      description = "The command to run when the service resumes";
      type = str;
      default = "echo 'service resumed'";
    };

    lockCmd = {
      description = "The command to run when the service locks";
      type = str;
      default = "echo 'lock!'";
    };

    unlockCmd = {
      description = "The command to run when the service unlocks";
      type = str;
      default = "echo 'unlock!'";
    };

    beforeSleepCmd = {
      description = "The command to run before the service sleeps";
      type = str;
      default = "echo 'Zzz...'";
    };
  };

  config = mkIf cfg.enable {
    xdg.configFile."hypr/hypridle.conf".text = ''
      listener {
        timeout = ${toString cfg.timeout};
        on-timeout = ${cfg.onTimeout}
        on-resume = ${cfg.onResume}
        lock_cmd = ${cfg.lockCmd}
        unlock_cmd = ${cfg.unlockCmd}
        before_sleep_cmd = ${cfg.beforeSleepCmd}
      }
    '';

    systemd.user.services.hypridle = {
      description = "Hypridle";
      after = ["graphical-session.target"];
      wantedBy = ["default.target"];
      serviceConfig = {
        ExecStart = "${getExe cfg.package}";
        Restart = "always";
        RestartSec = "10";
      };
    };
  };
}
