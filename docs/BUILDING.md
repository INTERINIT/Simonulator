# Building Simonulator

## Tested baseline

The overlay is tested with [MAME 0.289](../MAME_BASELINE.md), commit
`fbda0e727c853b7af6f4d2c3ebf297b807787900`, on 64-bit Windows.  The supplied
scripts install two overlay files into an upstream MAME source tree and build
a small `ibmsimon.exe` subtarget.

MAME changes its internal build system over time.  Use the pinned commit for a
reproducible result; newer MAME revisions are intentionally not claimed to be
compatible.

## Requirements

- 64-bit Windows 10 version 1809 or newer.
- PowerShell 7 or Windows PowerShell 5.1.
- Git.
- [MSYS2](https://www.msys2.org/) with the **UCRT64** environment.
- MAME build packages: `bash`, `git`, `make`,
  `mingw-w64-ucrt-x86_64-gcc`, and
  `mingw-w64-ucrt-x86_64-python`.
- A C++20-capable toolchain; the tested tree used GCC 16.2.0.  MAME documents
  GCC 11+ and Clang 13+ as supported minimums.
- Approximately 15 GB of free disk space and several GB of RAM are advisable
  for the MAME source tree and intermediate files.

The prerequisites follow MAME’s official
[compilation guide](https://docs.mamedev.org/initialsetup/compilingmame.html).

## 1. Install MSYS2 packages

Launch **MSYS2 UCRT64**, update it following MSYS2’s normal instructions, then
install the minimum compiler set:

```bash
pacman -S --needed base-devel git \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-python
```

The build uses MAME’s native Windows output.  SDL is optional and is not
required for the supplied command.

## 2. Get the exact MAME source

In a PowerShell or MSYS2 terminal:

```bash
git clone https://github.com/mamedev/mame.git
cd mame
git checkout fbda0e727c853b7af6f4d2c3ebf297b807787900
```

For example, this can produce `D:\src\mame` on Windows.

## 3. Build

From the root of this Simonulator repository in PowerShell:

```powershell
.\tools\Build-Simonulator.ps1 -MameRoot D:\src\mame -Msys2Root C:\msys64
```

The script:

1. copies `src/mame/ibm/simon.cpp` and `src/mame/layout/ibmsimon.lay` into the
   MAME checkout;
2. registers `ibmsimon` in `src/mame/mame.lst` idempotently; and
3. runs:

   ```bash
   make SUBTARGET=ibmsimon SOURCES=src/mame/ibm/simon.cpp REGENIE=1 -j<N>
   ```

The executable is `D:\src\mame\ibmsimon.exe`.  Use `-Jobs <N>` to cap
parallel compilation, for example `-Jobs 5`.

To perform only the source install step:

```powershell
.\tools\Install-MameOverlay.ps1 -MameRoot D:\src\mame
```

## 4. Supply firmware without committing it

Create a directory outside this repository, for example:

```text
D:\firmware\ibmsimon\simonbios.bin
D:\firmware\ibmsimon\simonflash.bin
```

The files must be legal dumps you are authorised to use.  They are excluded by
the repository’s `.gitignore`.  Do not publish their hashes as a substitute
for validating provenance.

## 5. Run

One standalone instance:

```powershell
.\tools\Start-Simon.ps1 -MameRoot D:\src\mame -FirmwareRoot D:\firmware
```

For a local virtual network, first run:

```powershell
.\tools\Start-CellularSwitch.ps1
```

Then launch two instances, each with:

```powershell
.\tools\Start-Simon.ps1 -MameRoot D:\src\mame -FirmwareRoot D:\firmware -CellularEndpoint socket.127.0.0.1:5555
```

The switch allocates virtual numbers in connection order, starting at 1001.

## Troubleshooting

- **`bash.exe` not found**: pass the installed MSYS2 root with `-Msys2Root`.
- **Driver does not appear/build**: rerun the build script.  It uses
  `REGENIE=1` because MAME requires generated project files to be refreshed
  when a driver is added.
- **Linker errors after changing source subsets**: remove MAME’s generated
  `build` directory for this subtarget, then rebuild.  This directory belongs
  to the MAME checkout, not this repository.
- **No service**: start the switch before the emulator and pass the exact
  `socket.127.0.0.1:5555` endpoint.
