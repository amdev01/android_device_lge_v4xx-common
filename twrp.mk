# TWRP Flags
TW_THEME := portrait_hdpi
BOARD_SUPPRESS_SECURE_ERASE := true
RECOVERY_SDCARD_ON_DATA := true
TW_NO_REBOOT_BOOTLOADER := true
TW_INCLUDE_CRYPTO := true
TW_NO_USB_STORAGE := true
TW_BRIGHTNESS_PATH := /sys/class/leds/lcd-backlight/brightness
RECOVERY_GRAPHICS_USE_LINELENGTH := true
TW_IGNORE_MAJOR_AXIS_0 := true
TW_NO_SCREEN_TIMEOUT := true

TARGET_RECOVERY_DEVICE_DIRS += device/lge/v4xx-common/twrp

BOARD_KERNEL_CMDLINE += androidboot.selinux=permissive