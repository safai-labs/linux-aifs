AIFS_VERSION="0.2.2"

EXTRA_CFLAGS += -DAIFS_VERSION=\"$(AIFS_VERSION)\"

ifeq ($(CONFIG_AIFS_LEGACY_READ),y)
  EXTRA_CFLAGS += -DCONFIG_AIFS_LEGACY_READ=1
else
  EXTRA_CFLAGS += -DCONFIG_AIFS_LEGACY_READ=0
endif
ifeq ($(CONFIG_AIFS_DEBUG),y)
  EXTRA_CFLAGS += -DAIFS_DEBUG
endif

obj-$(CONFIG_AIFS) += aifs.o

aifs-y := dir.o dentry.o file.o inode.o main.o super.o lookup.o mmap.o ovl_util.o
