AIFS_VERSION="2.0.5-2018"

EXTRA_CFLAGS += -DAIFS_VERSION=\"$(AIFS_VERSION)\" 

ifeq ($(CONFIG_AIFS_LEGACY_READ),y)
  EXTRA_CFLAGS += -DCONFIG_AIFS_LEGACY_READ=1
else
  EXTRA_CFLAGS += -DCONFIG_AIFS_LEGACY_READ=0
endif
ifeq ($(CONFIG_AIFS_DEBUG),y)
  EXTRA_CFLAGS += -DAIFS_DEBUG
endif


obj-$(CONFIG_AIFS_FS) += aifs.o
extra-$(CONFIG_AIFS_FS) += $(BUILD_NUMBER_FILE)

aifs-y := dir.o dentry.o file.o inode.o main.o super.o lookup.o mmap.o ovl_util.o

# Build number file.  Increment if any object file changes.
$(BUILD_NUMBER_FILE): $(aifs-y)
	@if ! test -f $(BUILD_NUMBER_FILE); then echo 0 > $(BUILD_NUMBER_FILE); fi
	@echo $$(($$(cat $(BUILD_NUMBER_FILE)) + 1)) > $(BUILD_NUMBER_FILE)

