# Ratcom — Release Protocol

## Pre-Release Checklist

1. All changes committed and pushed to `main`
2. Local build succeeds: `pio run -e ratputer_915`
3. Flash and test on device — confirm boot completes and basic functionality works
4. Version bumped in `src/config/Config.h` (all 4 defines: `RATPUTER_VERSION_MAJOR`, `RATPUTER_VERSION_MINOR`, `RATPUTER_VERSION_PATCH`, `RATPUTER_VERSION_STRING`)

## Release Steps

```bash
# 1. Commit version bump
git add src/config/Config.h
git commit -m "vX.Y.Z: description of changes"

# 2. Push to main
git push origin main

# 3. Create and push tag
git tag vX.Y.Z
git push origin vX.Y.Z

# CI automatically builds and creates GitHub release with ratcom-firmware.zip
```

## Post-Release Verification

1. Check [GitHub Actions](https://github.com/ratspeak/ratcom/actions) — both build and release jobs should pass
2. Verify the [release page](https://github.com/ratspeak/ratcom/releases) has `ratcom-firmware.zip` attached
3. Download the ZIP and confirm it contains:
   - `bootloader.bin`
   - `partitions.bin`
   - `boot_app0.bin`
   - `firmware.bin`
   - `manifest.json`
4. Web flasher at [ratspeak.org/download](https://ratspeak.org/download.html) should pick up the new release

## Hotfix Protocol

For critical bugs in a released version:

1. Fix on `main` branch
2. Bump patch version (e.g., 1.5.6 → 1.5.7)
3. Follow normal release steps above

## Build Environment Reference

| Parameter | Value |
|-----------|-------|
| PlatformIO env | `ratputer_915` |
| Version defines | `RATPUTER_VERSION_MAJOR/MINOR/PATCH/STRING` |
| Firmware artifact | `ratcom-firmware.zip` |
| Flash size | 8MB |
| Flash mode | dio |
