{
  description = "Meshtastic Gateway — bridge a connected Meshtastic node's channels to/from Logos Messaging";

  inputs = {
    # Pinned to the rev known-compatible with the current logoscore/logos_host.
    logos-module-builder.url = "github:logos-co/logos-module-builder/434b98ade6353efdae90083b00f20c8a8ba50ad7";
    # Local clone — `path:` picks up the working-tree metadata.json (universal/codegen dropped,
    # `events` kept) so the dependency exposes messageReceived without the broken universal codegen.
    delivery_module.url = "path:/home/vpavlin/devel/github.com/vpavlin/logos-delivery-module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
