# Windows Copilot-key provider for aqua

Makes the dedicated **Copilot key** on Windows keyboards open aqua,
natively through Windows Settings — no third-party remappers, hooks
or background apps.

Windows only lists apps in the Copilot-key picker if they are
MSIX-packaged, signed, and registered as a Copilot hardware-key
provider (`com.microsoft.windows.copilotkeyprovider` app extension,
Windows 11 build 22621+). This directory packages aqua exactly that
way, with a small launcher in place of a GUI.

## What the launcher does

- aqua **not running** → starts `aqua.exe` on your music folder
- aqua **running** → focuses its terminal window (restores it if
  minimized) and exits — never a second instance, since two aquas
  would be two ffplay streams fighting over the speakers

## Requirements

- Windows 11 22621+ with a Copilot key
- Windows SDK (provides `makeappx` / `signtool`)
- aqua's Windows exe somewhere (default
  `C:\Users\...\AppData\Local\Programs\aqua\aqua.exe`)

## Build & install

```powershell
cd windows-copilot-key
.\build.ps1                       # defaults
.\build.ps1 -AquaExe D:\aqua\aqua.exe -MusicDir D:\Music
```

`build.ps1` compiles the launcher with the on-box .NET compiler,
packs it with the SDK tools, signs it with a self-signed cert,
trusts that cert (one UAC prompt) and installs the package.

Then bind the key (Settings UI is the only writer Windows allows
for this):

**Settings → Personalization → Text input → Customize Copilot key
on keyboard → Custom → aqua**
(or Settings → Bluetooth & devices → Keyboard on some builds)

## Notes

- The manifest's `Publisher` must match the signing certificate's
  subject (`CN=aqua-player`) — `build.ps1` keeps them in sync
- Self-signed cert: trusted once via the UAC step; fine for a
  personal machine, use a real cert to distribute
- Rebuild when aqua's exe path changes or after updating aqua
- This wraps the Windows build of aqua; the player itself in
  `aqua.c` is unchanged
