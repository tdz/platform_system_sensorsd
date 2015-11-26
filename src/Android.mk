LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_SRC_FILES:= io.c \
                  main.c \
                  pdu.c \
                  poll.c \
                  receiver_svc.cpp \
                  registry.c \
                  service.c
LOCAL_C_INCLUDES := system/libfdio/include \
                    system/libpdu/include \
                    system/sensorsd/include
LOCAL_CFLAGS := -DANDROID_VERSION=$(PLATFORM_SDK_VERSION) -Wall -Werror
LOCAL_SHARED_LIBRARIES := libpdu \
                          libfdio \
                          libhardware \
                          libhardware_legacy \
                          libkeystore_binder \
                          libutils \
                          liblog \
                          libbinder
LOCAL_MODULE:= sensorsd
LOCAL_MODULE_PATH := $(TARGET_OUT_EXECUTABLES)
LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)
