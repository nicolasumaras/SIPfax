# CT105 native deployment provenance

## Current deployment: 70614d7

The current installed native binary is `/opt/sipfax/vendor/linmodem/lm`, built from commit `70614d7`, with SHA-256 `62b8190eed86cb4267447229d9ac1e8af0af0e4cdac93c67dda77204ae922623`. CT105 retains `/opt/sipfax/releases/native-70614d7-source.tar.gz` and `/opt/sipfax/releases/native-70614d7.json`. V.42/LAPM is enabled by `/etc/systemd/system/sipfax.service.d/v90-lapm.conf` with `Environment=SIPFAX_V90_V42=1` under `[Service]`.

Physical Windows XP calls have authenticated PPP and passed public internet probes at a reported 49,296 bit/s. See `v42-hardware-acceptance.json` for the accepted calls and known failures. The next-phase campaign tests this unchanged native build. Short-call success does not yet qualify the build for sustained reliability or multiple simultaneous calls.

The native rollback artifact is `/opt/sipfax/releases/lm-pre-lapm-fbaeb83.rollback`, SHA-256 `cac0ea0c9f93a4ce2d4479b339047ec019a3709e499f95e44d4f8cfe5d456c7c`. Restoring this pre-LAPM binary also requires removing the `v90-lapm.conf` drop-in, then reloading systemd and restarting SIPfax. This is a native-modem rollback, not a complete application/configuration rollback.

The application checkout under `/opt/sipfax` contains deployment overlays. Native source provenance does not by itself identify all running JavaScript. The subsequent PPP/modem process lifecycle fixes on the development branch have not been deployed during baseline collection. Full application deployment reproducibility and rollback qualification remain release gates.

## Historical deployment: 8aa8c5e

The following record describes the earlier deployment, not the currently installed binary.

The installed native binary is built from commit `8aa8c5e5a5af389aa3f060a50d668498adede909`, with SHA256 `cac0ea0c9f93a4ce2d4479b339047ec019a3709e499f95e44d4f8cfe5d456c7c`. The service uses an initial upstream ceiling of 28800, automatic initial echo acquisition and a downstream ceiling of 49334. Both upstream symbol rates are offered according to caller capabilities. Causal ODP training is enabled, with index-2 pre-emphasis requested for 3000 symbols/s. Both sustained hardware profiles passed all transfer hashes, with residual CRC errors documented in `upstream-rates.md`.

The historical `/opt/sipfax` Git checkout predates the deployment overlays. All fourteen deployed JavaScript source files matched the development branch in the 2026-09-11 audit; its top-level Git HEAD alone does not identify the running native code.

CT105 now retains the native source archive and a build record independently of the temporary build directory:

- `/opt/sipfax/releases/native-8aa8c5e-source.tar`: tracked `vendor/linmodem` source from the pinned commit.
- `/opt/sipfax/releases/native-8aa8c5e.json`: commit, archive hash, installed binary hash, compiler version, build directory and compiler flags.

Both retained files were checked against their local SHA256 values after installation. Rebuild in a separate directory:

```sh
native_rebuild_dir="$(mktemp -d /tmp/sipfax-native-rebuild.XXXXXX)"
tar -xf /opt/sipfax/releases/native-8aa8c5e-source.tar -C "$native_rebuild_dir"
make -C "$native_rebuild_dir/vendor/linmodem" CFLAGS='-O2 -Wall -g -D_GNU_SOURCE -fcommon'
```

A rebuild in a different directory or compiler environment can have a different ELF hash, including from debug paths. Qualify changes before replacing the active binary; the retained installed-binary hash identifies the tested artifact.

The previous `9be7827` binary is retained at `/opt/sipfax/releases/lm-9be7827.rollback`, with its service experiment configuration at `/opt/sipfax/releases/v90-upstream-9be7827.rollback.conf`. Its SHA256 is `1195c6f8b6413e0b72c5822283a5820c4d38f19a4508ae1b8cebc81872c5b14a`. Stop the service before restoring both files, then daemon-reload and start it. Persistent application settings, PPP users and credentials live separately and are not included in source archives or build records.

After retention, attempt `bab00053-d4e9-4ee1-b3d4-a9ab3d94859d` passed internet access and all 18 hashes at 3200/28800, downstream 49.333, with zero modem errors over 35.932 seconds. Forwarded RTP audio matches exactly with no gaps/drops; one final server packet after ATA BYE was not forwarded. The service remained on the retained binary after the call.
