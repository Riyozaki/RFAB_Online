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

# Freeze this before dependency .d files extend MAKEFILE_LIST; late recipe expansion must
# not reinterpret a generated header dependency file as the repository's own Makefile.
REPO_ROOT ?= $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../..)
REPO_ROOT := $(REPO_ROOT)

# SANITIZE=1 rebuilds the same suites with AddressSanitizer + UndefinedBehaviorSanitizer
# into their own object tree. Use it when a test passes but the behaviour looks wrong:
# it is how the dangling TLV chunk views would have been caught without a second platform.
ifeq ($(SANITIZE),1)
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
  BUILD_SUFFIX := -asan
  # Vendored sources keep AddressSanitizer but drop the UB checks: TiltedCore's
  # ReadVarInt shifts by more than 64 bits on a malformed varint
  # (Code/core/src/Serialization.cpp:34). That is an upstream bug we cannot fix from
  # here and it would fail every sanitized run; our own code stays fully checked.
  VENDOR_SAN := -fno-sanitize=undefined
  # TiltedCore's allocators (every TiltedPhoques::Vector, Map, String, Buffer) take their memory
  # from mimalloc, whose blocks AddressSanitizer does not see on its own. MI_TRACK_ASAN makes
  # mimalloc poison what it frees, so a use after free of such a container is reported.
  MIMALLOC_SAN := -fsanitize=address -fno-omit-frame-pointer -DMI_TRACK_ASAN=1
endif

BUILD_DIR ?= $(REPO_ROOT)/build/linux-core$(BUILD_SUFFIX)
DEPS_DIR  ?= $(REPO_ROOT)/build/deps
OBJ_DIR   := $(BUILD_DIR)/obj
BIN       := $(BUILD_DIR)/coretests

CXX      ?= g++
CC       ?= gcc
OPT      ?= -O1
CXXFLAGS += -std=c++20 $(OPT) -fPIC -DNOMINMAX -Wno-everything $(SAN_FLAGS)

# Header dependency tracking. Without it, editing a header rebuilds nothing and the
# loop reports a pass for code it did not compile - the exact failure this harness
# exists to prevent.
DEPFLAGS := -MMD -MP
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
  -I$(REPO_ROOT)/Code/client \
  -I$(REPO_ROOT)/Code/tests \
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
  $(REPO_ROOT)/Code/tests/rfab_health_inventory.cpp \
  $(REPO_ROOT)/Code/tests/rfab_coop.cpp \
  $(REPO_ROOT)/Code/tests/playback_clock.cpp \
  $(REPO_ROOT)/Code/client/Systems/PlaybackClock.cpp \
  $(REPO_ROOT)/Code/tests/rfab_wire_fuzz.cpp \
  $(REPO_ROOT)/Code/tests/rfab_v410_compat_overlay.cpp \
  $(REPO_ROOT)/Code/tests/rfab_v411_breadcrumbs.cpp \
  $(REPO_ROOT)/Code/tests/rfab_v411_safe_fs.cpp

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
# Every public co-op boundary is also parsed as the FIRST include in its own TU. A PCH
# in an unrelated target must never make a header appear self-contained when it is not.
STANDALONE_HEADERS := $(wildcard $(REPO_ROOT)/Code/common/Coop/*.h) \
  $(addprefix $(REPO_ROOT)/Code/encoding/Structs/,GameId.h Inventory.h MagicEquipment.h)
header_check = $(BUILD_DIR)/headers/$(call rel_name,$(1)).ok
HEADER_CHECKS := $(foreach h,$(STANDALONE_HEADERS),$(call header_check,$(h)))
ALL_DEPS := $(ALL_OBJS:.o=.d) $(HEADER_CHECKS:.ok=.d)

.PHONY: all headers run clean
all: $(BIN) $(HEADER_CHECKS)
headers: $(HEADER_CHECKS)

run: all
	@$(BIN) -r compact

$(BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD    $(notdir $@)"
	@$(CXX) $(SAN_FLAGS) $(ALL_OBJS) -o $@ -lpthread

# Every object also depends on this Makefile. Flags, defines and include paths live
# here, and make only tracks *source* prerequisites (-MMD -MP). Without this, editing
# CXXFLAGS / SANITIZE / a -D define left stale objects compiled under the old flags,
# and the loop reported a result about code it never rebuilt. A Makefile edit is rare
# and a full rebuild is the correct, cheap price for never lying about what was built.
$(MIMALLOC_OBJ): $(MIMALLOC)/src/static.c $(MAKEFILE_LIST)
	@mkdir -p $(dir $@)
	@echo "  CC    mimalloc"
	@# -U, not -D...=0: mimalloc guards the malloc interposer with #if defined(), so
	@# defining it to zero switched the override ON and broke AddressSanitizer.
	@$(CC) $(CFLAGS) $(MIMALLOC_SAN) -w -UMI_MALLOC_OVERRIDE -I$(MIMALLOC)/include -c $< -o $@

define compile_rule
$(call obj_of,$(1),$(2)): $(2) $(MAKEFILE_LIST)
	@mkdir -p $$(dir $$@)
	@echo "  CXX   $$(patsubst $$(REPO_ROOT)/%,%,$$<)"
	@$$(CXX) $$(CXXFLAGS) $$(DEPFLAGS) -w $$(INCLUDES) $(3) -c $$< -o $$@
endef

$(foreach s,$(CORE_SRCS),$(eval $(call compile_rule,core,$(s),$(VENDOR_SAN))))
$(foreach s,$(ENCODING_SRCS),$(eval $(call compile_rule,encoding,$(s),$(PCH))))
# CommonLib has NO PCH in the shipping xmake target. Do not hide its transitive includes
# behind EncodingPch.h (Windows run44140814679 caught missing GameId/Inventory includes).
$(foreach s,$(COMMON_SRCS),$(eval $(call compile_rule,common,$(s),)))
# The suites are compiled WITHOUT the encoding prefix header, exactly like the xmake TPTests
# target: a suite that only compiles because EncodingPch.h was force-included builds here and
# fails under MSVC (first Windows CI run: rfab_v411_phase3_adaptation_tests.cpp, 38 errors).
$(foreach s,$(PORTABLE_TESTS),$(eval $(call compile_rule,tests,$(s),)))

define header_rule
$(call header_check,$(1)): $(1) $(MAKEFILE_LIST)
	@mkdir -p $$(dir $$@)
	@echo "  HDR   $$(patsubst $$(REPO_ROOT)/%,%,$$<)"
	@$$(CXX) -std=c++20 -DNOMINMAX -w $$(INCLUDES) -I$$(REPO_ROOT)/Tools/rfab/linux-server/stub -x c++ -fsyntax-only -MMD -MP -MF $$(@:.ok=.d) -MT $$@ $$<
	@touch $$@
endef
$(foreach h,$(STANDALONE_HEADERS),$(eval $(call header_rule,$(h))))

-include $(ALL_DEPS)

clean:
	rm -rf $(BUILD_DIR)
