# Fast, platform-independent test loop for RFAB Online.
#
# Builds TiltedCore + Code/encoding + Code/common + the portable Catch2 suites with a plain
# compiler, so protocol and coop-core logic can be compiled and executed in seconds instead of
# waiting 13-19 minutes for the Windows release pipeline.
#
# Usage (from the repository root):
#   Tools/rfab/run-core-tests.sh            # fetch deps, build, run
#   make -C Tools/rfab/linux-core -j$(nproc) run
#
# Scope: everything here is OS independent. The Skyrim client (hooks, D3D11, CEF) and the
# GameNetworkingSockets transport are NOT part of this target - they stay on the xmake build.

REPO_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
BUILD_DIR ?= $(REPO_ROOT)/build/linux-core
DEPS_DIR  ?= $(REPO_ROOT)/build/deps
OBJ_DIR   := $(BUILD_DIR)/obj
BIN       := $(BUILD_DIR)/coretests

CXX      ?= g++
CC       ?= gcc
OPT      ?= -O1
CXXFLAGS += -std=c++20 $(OPT) -fPIC -DNOMINMAX -Wno-everything
CFLAGS   += -std=c11 $(OPT) -fPIC

TILTEDCORE := $(DEPS_DIR)/TiltedCore-0.2.9
HOPSCOTCH  := $(DEPS_DIR)/hopscotch-map-2.3.1
GLM        := $(DEPS_DIR)/glm-0.9.9.8
CATCH2     := $(DEPS_DIR)/Catch2-2.13.10
MIMALLOC   := $(DEPS_DIR)/mimalloc-2.2.4

INCLUDES := \
  -I$(REPO_ROOT)/Code \
  -I$(REPO_ROOT)/Code/encoding \
  -I$(REPO_ROOT)/Code/encoding/Structs/Skyrim \
  -I$(REPO_ROOT)/Code/common \
  -I$(DEPS_DIR)/tcinc \
  -I$(TILTEDCORE)/Code/core/include \
  -I$(HOPSCOTCH)/include \
  -I$(GLM) \
  -I$(CATCH2)/single_include \
  -I$(MIMALLOC)/include

PCH := -include $(REPO_ROOT)/Code/encoding/EncodingPch.h

# Suites that do not depend on Windows, the Skyrim client or GameNetworkingSockets.
# Add new portable suites here; everything else keeps running in the xmake TPTests target.
PORTABLE_TESTS := \
  $(REPO_ROOT)/Code/tests/main.cpp \
  $(REPO_ROOT)/Code/tests/encoding.cpp \
  $(REPO_ROOT)/Code/tests/rfab_wire_fuzz.cpp \
  $(REPO_ROOT)/Code/tests/rfab_string_cache_protocol.cpp

CORE_SRCS     := $(filter-out %MimallocAllocator.cpp,$(wildcard $(TILTEDCORE)/Code/core/src/*.cpp)) \
                 $(TILTEDCORE)/Code/core/src/MimallocAllocator.cpp
ENCODING_SRCS := $(shell find $(REPO_ROOT)/Code/encoding -name '*.cpp' 2>/dev/null)
COMMON_SRCS   := $(shell find $(REPO_ROOT)/Code/common -name '*.cpp' 2>/dev/null)

# Object names keep the full relative path (flattened) - several sources share a basename
# (e.g. two AnimationGraphDescriptorManager.cpp), which silently clobbered objects otherwise.
rel_name = $(subst /,_,$(patsubst $(DEPS_DIR)/%,%,$(patsubst $(REPO_ROOT)/%,%,$(basename $(1)))))
obj_of = $(OBJ_DIR)/$(1)/$(call rel_name,$(2)).o
CORE_OBJS     := $(foreach s,$(CORE_SRCS),$(call obj_of,core,$(s)))
ENCODING_OBJS := $(foreach s,$(ENCODING_SRCS),$(call obj_of,encoding,$(s)))
COMMON_OBJS   := $(foreach s,$(COMMON_SRCS),$(call obj_of,common,$(s)))
TEST_OBJS     := $(foreach s,$(PORTABLE_TESTS),$(call obj_of,tests,$(s)))
MIMALLOC_OBJ  := $(OBJ_DIR)/mimalloc.o

ALL_OBJS := $(CORE_OBJS) $(ENCODING_OBJS) $(COMMON_OBJS) $(TEST_OBJS) $(MIMALLOC_OBJ)

.PHONY: all run clean
all: $(BIN)

run: $(BIN)
	@$(BIN) -r compact

$(BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD    $(notdir $@)"
	@$(CXX) $(ALL_OBJS) -o $@ -lpthread

$(MIMALLOC_OBJ): $(MIMALLOC)/src/static.c
	@mkdir -p $(dir $@)
	@echo "  CC    mimalloc"
	@$(CC) $(CFLAGS) -w -DMI_MALLOC_OVERRIDE=0 -I$(MIMALLOC)/include -c $< -o $@

define compile_rule
$(call obj_of,$(1),$(2)): $(2)
	@mkdir -p $$(dir $$@)
	@echo "  CXX   $$(patsubst $$(REPO_ROOT)/%,%,$$<)"
	@$$(CXX) $$(CXXFLAGS) -w $$(INCLUDES) $(3) -c $$< -o $$@
endef

$(foreach s,$(CORE_SRCS),$(eval $(call compile_rule,core,$(s),)))
$(foreach s,$(ENCODING_SRCS),$(eval $(call compile_rule,encoding,$(s),$(PCH))))
$(foreach s,$(COMMON_SRCS),$(eval $(call compile_rule,common,$(s),$(PCH))))
$(foreach s,$(PORTABLE_TESTS),$(eval $(call compile_rule,tests,$(s),$(PCH))))

clean:
	rm -rf $(BUILD_DIR)
