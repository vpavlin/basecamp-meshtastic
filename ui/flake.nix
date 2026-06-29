{
  description = "Mesh gateway UI — choose which mesh channels to bridge to Logos Messaging";

  inputs = {
    # Same pinned rev as gateway/flake.nix — logos-basecamp 0.2.0-RC3's module-builder (ABI match).
    logos-module-builder.url = "github:logos-co/logos-module-builder/b0e41abf3e14c0534b41933c5f8e3fc697319037";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
