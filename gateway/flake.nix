{
  description = "Meshtastic Gateway — bridge a connected Meshtastic node's channels to/from Logos Messaging";

  inputs = {
    # Pinned to logos-basecamp 0.2.0-RC3's primary module-builder rev (ABI match for that release).
    logos-module-builder.url = "github:logos-co/logos-module-builder/b0e41abf3e14c0534b41933c5f8e3fc697319037";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
