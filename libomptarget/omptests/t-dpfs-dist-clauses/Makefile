include ../Makefile.defs

ifeq ($(OMPTESTS_CUDA_COMPUTE_CAPABILITY),60)
        EXTRAFLAGS += --cuda-noopt-device-debug
endif

EXTRAFLAGS += -O3

################################################################################
#
# Include any special rule for this testcase here! Usually:
#
# static:
#
# dynamic:
#
################################################################################

include ../Makefile.rules
