/*
 * GZ302 rear camera: let the stock amdgpu find the camera without a kernel patch (findings 136).
 *
 * amdgpu creates the ISP (and its I2C and GPIO controllers) only if amdgpu_acpi_get_isp4_dev() finds a platform
 * device matching its allow-list, which has only "OMNI5C10" (the HP ZBook Ultra G1a sensor). The match uses
 * acpi_match_device(), which also checks a device's compatible IDs (_CID). The GZ302's rear camera \_SB.CAMB has
 * _HID "OMNI13B1" and no _CID (SSDT23). Adding _CID "OMNI5C10" makes amdgpu find it; the _HID stays "OMNI13B1", so
 * amd_isp4 still picks its OV13B10 configuration (the device's _HID is matched before its _CID) and amdgpu's
 * HP-only GPIO tables are not used (they are keyed on the _HID string).
 *
 * Loaded at boot from the early initramfs by mkinitcpio's acpi_override hook (CONFIG_ACPI_TABLE_UPGRADE).
 * Shipped by the gz302-camera-dkms package (scripts/make-aur-dkms.sh compiles it with iasl) as
 * /usr/lib/initcpio/acpi_override/gz302-camb-cid.aml, with the drop-in acpi/90-gz302-camera-acpi.conf; installing or
 * removing the package rebuilds every initramfs through the mkinitcpio pacman hook.
 * Manual build: iasl -p acpi/gz302-camb-cid acpi/gz302-camb-cid.asl
 */
DefinitionBlock ("", "SSDT", 2, "GZ302", "CAMBCID", 0x00000001)
{
    External (\_SB.CAMB, DeviceObj)

    Scope (\_SB.CAMB)
    {
        Name (_CID, "OMNI5C10")
    }
}
