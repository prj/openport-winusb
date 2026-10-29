# OpenPort 2.0 on WinUSB

The Tactrix OpenPort 2.0 J2534 interface stopped working on Windows 11. Its kernel driver, `openport.sys`, is
cross-signed, and Windows refuses to load it (Device Manager shows Code 39). Tactrix has not offered a fix.

This project keeps Tactrix's own J2534 DLL, which still works, and replaces only the driver:

- `driver/` binds the device to Microsoft's inbox `winusb.sys` under the original driver's interface GUID, so
  the unmodified DLL still finds the device.
- `shim/` is installed as `op20pt32.dll` and loads Tactrix's DLL, renamed to `op20pt32_tactrix.dll`. It hooks
  the five ntdll file calls the DLL makes on the device handle and performs them over WinUSB, behaving like
  `openport.sys` did: ten 64-byte reads always pending into a 16 KB ring, and reads completed as soon as any
  data is buffered.
- `installer/` is an NSIS installer that puts everything in place and keeps Tactrix's J2534 registry entry, so
  programs need no reconfiguration. Uninstall restores Tactrix's DLL.

Tested on Windows 11 with driver signature enforcement on: a full ECU flash through J2534-Sharp, DiagRA-D, and a
20,000-frame CAN loopback with no frames lost, all matching the original driver's timing.

## Building

Needs Visual Studio with the x86 C++ tools, the Windows SDK and NSIS. `installer\build.ps1` builds, signs and
packages everything. Signing uses Azure Artifact Signing through `tools\sign.ps1`, which reads the dlib and
metadata paths from `tools\sign.local.json`; adapt both to your own certificate.

Tactrix's DLL is not in this repository. The build takes it from an installed copy of Tactrix's last release
(`openport2_setup_1024820.exe`, DLL version 1.02.4820).

## Troubleshooting

Create an empty `op20pt32.log` next to the installed `op20pt32.dll` (`C:\Windows\SysWOW64` on 64-bit Windows) and
the shim logs device opens and errors to it.

## Licence

MIT, see `LICENSE`. Tactrix's DLL is distributed unmodified under Tactrix's own licence.
