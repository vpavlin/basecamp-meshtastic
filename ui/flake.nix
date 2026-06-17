{
  description = "Meshtastic Gateway UI — choose which Meshtastic channels to bridge to Logos Messaging";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    meshtastic_gateway.url = "path:/home/vpavlin/devel/github.com/vpavlin/basecamp-meshtastic/gateway";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
