# CT105 native deployment provenance

The installed native binary is built from commit `9be7827a9346a9d2c73dcc63ab6838ffb8af1637`, with SHA256 `1195c6f8b6413e0b72c5822283a5820c4d38f19a4508ae1b8cebc81872c5b14a`. The service uses an initial upstream ceiling of 28800, automatic initial echo acquisition and a downstream ceiling of 49334. Both upstream symbol rates are offered according to caller capabilities.

The historical `/opt/sipfax` Git checkout predates the deployment overlays. All fourteen deployed JavaScript source files matched the development branch in the 2026-09-11 audit; its top-level Git HEAD alone does not identify the running native code.

CT105 now retains the native source archive and a build record independently of the temporary build directory:

- `/opt/sipfax/releases/native-9be7827-source.tar`: tracked `vendor/linmodem` source from the pinned commit.
- `/opt/sipfax/releases/native-9be7827.json`: commit, archive hash, installed binary hash, compiler version, build directory and compiler flags.

Both retained files were checked against their local SHA256 values after installation. Rebuild in a separate directory:

```sh
native_rebuild_dir="$(mktemp -d /tmp/sipfax-native-rebuild.XXXXXX)"
tar -xf /opt/sipfax/releases/native-9be7827-source.tar -C "$native_rebuild_dir"
make -C "$native_rebuild_dir/vendor/linmodem" CFLAGS='-O2 -Wall -g -D_GNU_SOURCE -fcommon'
```

A rebuild in a different directory or compiler environment can have a different ELF hash, including from debug paths. Qualify changes before replacing the active binary; the retained installed-binary hash identifies the tested artifact.

The previous binary is retained at `/opt/sipfax/vendor/linmodem/lm.pre-9be7827`, with its service experiment configuration at `/tmp/v90-upstream-before-9be7827.conf`. These support rollback of the native deployment. Persistent application settings, PPP users and credentials live separately and are not included in the source archive or build record.
