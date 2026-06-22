{
  description = "Meshtastic Gateway — bridge a connected Meshtastic node's channels to/from Logos Messaging";

  inputs = {
    # Pinned to the rev known-compatible with the current logoscore/logos_host.
    logos-module-builder.url = "github:logos-co/logos-module-builder/434b98ade6353efdae90083b00f20c8a8ba50ad7";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
