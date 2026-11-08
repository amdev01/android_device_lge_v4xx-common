LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := irrc_hal_tx
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := irrc_hal_tx.c
LOCAL_SHARED_LIBRARIES := libhardware libcutils liblog
LOCAL_C_INCLUDES := hardware/libhardware/include
include $(BUILD_EXECUTABLE)
