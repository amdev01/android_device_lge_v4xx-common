#
# Copyright (C) 2015 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# inherit from the proprietary version
-include vendor/lge/v4xx-common/BoardConfigVendor.mk

LOCAL_PATH := device/lge/v4xx-common

# Platform
TARGET_BOARD_PLATFORM := msm8226

# CPU
TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_MEMCPY_BASE_OPT_DISABLE := true
TARGET_CPU_VARIANT := krait

# Bluetooth
BOARD_HAVE_BLUETOOTH_QCOM := true
BLUETOOTH_HCI_USE_MCT := true
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := device/lge/v4xx-common/bluetooth

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := MSM8226
TARGET_NO_BOOTLOADER := true

# Kernel
BOARD_CUSTOM_BOOTIMG := true
BOARD_CUSTOM_BOOTIMG_MK := $(LOCAL_PATH)/mkbootimg.mk
BOARD_KERNEL_CMDLINE := console=none user_debug=31 msm_rtb.filter=0x37 androidboot.hardware=v4xx
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_SEPARATED_DT := true
BOARD_MKBOOTIMG_ARGS := --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 --tags_offset 0x00000100
KERNEL_TOOLCHAIN := $(ANDROID_BUILD_TOP)/prebuilts/gcc/$(HOST_OS)-x86/arm/arm-eabi-4.8/bin
TARGET_KERNEL_CROSS_COMPILE_PREFIX := arm-eabi-
TARGET_KERNEL_SOURCE := kernel/lge/v4xx

# Audio
BOARD_USES_ALSA_AUDIO := true
AUDIO_FEATURE_ENABLED_MULTI_VOICE_SESSIONS := true
AUDIO_FEATURE_ENABLED_NEW_SAMPLE_RATE := true
AUDIO_FEATURE_ENABLED_COMPRESS_VOIP := false
USE_CUSTOM_AUDIO_POLICY := 1

# Bluetooth
BOARD_HAVE_BLUETOOTH := true

# Camera
USE_DEVICE_SPECIFIC_CAMERA := true
TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true
TARGET_HAS_LEGACY_CAMERA_HAL1 := true
BOARD_GLOBAL_CFLAGS += -DCAMERA_VENDOR_L_COMPAT

# CMHW
BOARD_HARDWARE_CLASS := $(LOCAL_PATH)/cmhw/
TARGET_TAP_TO_WAKE_NODE := "/sys/devices/virtual/input/lge_touch/tap_to_wake"

# Display
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
TARGET_USES_C2D_COMPOSITION := true
TARGET_USES_ION := true
USE_OPENGL_RENDERER := true

MAX_EGL_CACHE_KEY_SIZE := 12*1024
MAX_EGL_CACHE_SIZE := 2048*1024

HAVE_ADRENO_SOURCE:= false
OVERRIDE_RS_DRIVER:= libRSDriver_adreno.so
TARGET_USE_COMPAT_GRALLOC_PERFORM := true

# Fonts
EXTENDED_FONT_FOOTPRINT := true

# Include
TARGET_SPECIFIC_HEADER_PATH := $(LOCAL_PATH)/include

# Lights
TARGET_PROVIDES_LIBLIGHT := true

# Offmode Charging
BOARD_CHARGING_CMDLINE_NAME := "androidboot.mode"
BOARD_CHARGING_CMDLINE_VALUE := "chargerlogo"
BOARD_HEALTHD_CUSTOM_CHARGER_RES := $(LOCAL_PATH)/charger/images

# Partitions
# TODO
BOARD_BOOTIMAGE_PARTITION_SIZE := 0x01800000
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_CACHEIMAGE_PARTITION_SIZE := 947912704
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 0x01800000
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2147483648
BOARD_FLASH_BLOCK_SIZE := 131072
TARGET_USERIMAGES_USE_F2FS := true

# Power
TARGET_POWERHAL_VARIANT := qcom

# Qualcomm support
BOARD_USES_QCOM_HARDWARE := true

# Recovery
BOARD_HAS_NO_SELECT_BUTTON := true
BOARD_NO_SECURE_DISCARD := true
BOARD_RECOVERY_SWIPE := true
BOARD_SUPPRESS_EMMC_WIPE := true
BOARD_USE_CUSTOM_RECOVERY_FONT := \"roboto_23x41.h\"
TARGET_RECOVERY_FSTAB := device/lge/v4xx-common/rootdir/etc/fstab.v4xx
TARGET_RECOVERY_PIXEL_FORMAT := "RGBX_8888"
TARGET_USERIMAGES_USE_EXT4 := true

# RIL
BOARD_GLOBAL_CFLAGS += -DUSE_RIL_VERSION_10
BOARD_GLOBAL_CPPFLAGS += -DUSE_RIL_VERSION_10
TARGET_RIL_VARIANT := caf

# SDClang
TARGET_USE_SDCLANG := true

# SELinux
include device/qcom/sepolicy/sepolicy.mk
# msm8226/file_contexts labels misc as misc_block_device, which conflicts with
# our wcnss_block_device label required for user builds. Drop that chipset dir
# (it only contains file_contexts) and carry those labels in our sepolicy/.
BOARD_SEPOLICY_DIRS := $(filter-out device/qcom/sepolicy/$(TARGET_BOARD_PLATFORM),$(BOARD_SEPOLICY_DIRS))
BOARD_SEPOLICY_DIRS += device/lge/v4xx-common/sepolicy

# Time services
# Hardware qpnp RTC is not software-writable on this PMIC (SPMI write fails
# with EPERM even with qcom,qpnp-rtc-write=1). Stock approach is offset files.
#
# libtime_genoff.so (~5KB) is present but is only a socket client for missing
# time_daemon / TimeService — do NOT enable BOARD_USES_QC_TIME_SERVICES.
#
# Sony TimeKeep was removed: its restore used raw settimeofday and left
# /dev/alarm ELAPSED_REALTIME stale (issues 001/003).
#
# Replacement: device/lge/v4xx-common/timepersist — stores ats_2 +
# persist.sys.timeadjust; restores via ANDROID_ALARM_SET_RTC (updates alarm
# delta). TWRP uses TARGET_RECOVERY_QCOM_RTC_FIX to read ats_2 (no restore
# service in recovery).
# BOARD_USES_QC_TIME_SERVICES := true

# Wifi
BOARD_HAS_QCOM_WLAN := true
BOARD_WLAN_DEVICE := qcwcn
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
TARGET_USES_WCNSS_CTRL := true
TARGET_USES_QCOM_WCNSS_QMI := true
WIFI_DRIVER_FW_PATH_AP := "ap"
WIFI_DRIVER_FW_PATH_STA := "sta"
WPA_SUPPLICANT_VERSION := VER_0_8_X

ifeq ($(WITH_TWRP),true)
# TWRP-Specific
PRODUCT_COPY_FILES += $(LOCAL_PATH)/recovery/root/twrp.fstab:recovery/root/etc/twrp.fstab
TW_DEVICE_VERSION=1
RECOVERY_VARIANT := twrp
TW_THEME := portrait_hdpi
BOARD_SUPPRESS_SECURE_ERASE := true
RECOVERY_SDCARD_ON_DATA := true
TW_NO_REBOOT_BOOTLOADER := true
TW_INCLUDE_CRYPTO := true
TW_NO_USB_STORAGE := true
TW_BRIGHTNESS_PATH := /sys/class/leds/lcd-backlight/brightness
RECOVERY_GRAPHICS_USE_LINELENGTH := true
TW_IGNORE_MAJOR_AXIS_0 := true
TARGET_RECOVERY_QCOM_RTC_FIX := true
endif
