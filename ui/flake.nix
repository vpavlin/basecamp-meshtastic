{
  description = "Meshtastic Gateway UI — choose which Meshtastic channels to bridge to Logos Messaging";

  inputs = {
    # Same pinned rev as gateway/flake.nix (ABI-compatible with the current logos_host).
    logos-module-builder.url = "github:logos-co/logos-module-builder/434b98ade6353efdae90083b00f20c8a8ba50ad7";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
