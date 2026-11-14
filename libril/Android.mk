# Copyright 2006 The Android Open Source Project
#
# Device-provided libril (BOARD_PROVIDES_LIBRIL): CAF cm-14.1-caf base with
# Qualcomm/LG KitKat RIL_AppStatus retry fields so sizeof(RIL_CardStatus_v*)
# matches stock libril-qc-qmi-1.so (GET_SIM_STATUS INVALID_RESPONSE fix).

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    ril.cpp \
    ril_event.cpp \
    RilSocket.cpp \
    RilSapSocket.cpp \

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libbinder \
    libcutils \
    libhardware_legacy \
    librilutils \

LOCAL_STATIC_LIBRARIES := \
    libprotobuf-c-nano-enable_malloc \

ifeq ($(SIM_COUNT), 2)
    LOCAL_CFLAGS := -DANDROID_MULTI_SIM
    LOCAL_CFLAGS += -DANDROID_SIM_COUNT_2
endif

LOCAL_C_INCLUDES += external/nanopb-c
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include

LOCAL_MODULE:= libril
LOCAL_MODULE_TAGS := optional
ifeq ($(TARGET_NEEDS_GCC_LIBRIL),true)
    LOCAL_CLANG := false
else
    LOCAL_CLANG := true
    LOCAL_SANITIZE := integer
endif

include $(BUILD_SHARED_LIBRARY)
