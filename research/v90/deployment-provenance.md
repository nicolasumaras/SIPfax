# CT105 native deployment provenance

The installed native binary is built from commit `efc00dc911368d19217394c8977b5cfb0aacaeaf`, with SHA256 `4354c8ca7ed2d0ec1f2174889d5cbe2c7a6a246b071175780719ddfe77f077a6`. The service uses an initial upstream ceiling of 28800, automatic initial echo acquisition and a downstream ceiling of 49334. Both upstream symbol rates are offered according to caller capabilities.

The historical `/opt/sipfax` Git checkout predates the deployment overlays. All fourteen deployed JavaScript source files matched the development branch in the 2026-09-11 audit; its top-level Git HEAD alone does not identify the running native code.

CT105 now retains the native source archive and a build record independently of the temporary build directory:

- `/opt/sipfax/releases/native-efc00dc-source.tar`: tracked `vendor/linmodem` source from the pinned commit.
- `/opt/sipfax/releases/native-efc00dc.json`: commit, archive hash, installed binary hash, compiler version, build directory and compiler flags.

Both retained files were checked against their local SHA256 values after installation. Rebuild in a separate directory:

```sh
native_rebuild_dir="$(mktemp -d /tmp/sipfax-native-rebuild.XXXXXX)"
tar -xf /opt/sipfax/releases/native-efc00dc-source.tar -C "$native_rebuild_dir"
make -C "$native_rebuild_dir/vendor/linmodem" CFLAGS='-O2 -Wall -g -D_GNU_SOURCE -fcommon'
```

A rebuild in a different directory or compiler environment can have a different ELF hash, including from debug paths. Qualify changes before replacing the active binary; the retained installed-binary hash identifies the tested artifact.

The previous binary is retained at `/opt/sipfax/vendor/linmodem/lm.pre-efc00dc`, with its service experiment configuration at `/tmp/v90-upstream-before-efc00dc.conf`. These support rollback of the native deployment. Persistent application settings, PPP users and credentials live separately and are not included in the source archive or build record.
