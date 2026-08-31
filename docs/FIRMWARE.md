# Firmware policy and setup

Simonulator requires two files from an IBM Simon that you are authorised to
dump and use:

```text
<FirmwareRoot>\ibmsimon\simonbios.bin
<FirmwareRoot>\ibmsimon\simonflash.bin
```

The driver expects the lower-case filename `simonflash.bin` for the Flash
region.  Windows is usually case-insensitive, but keeping these names avoids
problems on other hosts.

These images are not source code for Simonulator and are not licensed by this
repository.  They are excluded from Git by `.gitignore`, and must not be
uploaded to GitHub, attached to releases, or included in pull requests.

The project has been validated with a 128 KiB BIOS dump and a 1 MiB Flash dump.
Different revisions may work, but no compatibility guarantee is made.  Keep
private backups of any original dumps outside the repository.
