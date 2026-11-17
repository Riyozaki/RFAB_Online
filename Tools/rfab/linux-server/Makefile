# Linux build of the *real* server (Code/server) and its in-process harness.
#
# Why this exists
# ---------------
# `Code/server` is 14k lines of platform-independent logic - ownership, damage, saves,
# parties - and until now it was compiled for the first time only inside the 16-minute
# Windows release pipeline, and executed only when a human played the game. Every server
# side defect therefore had a feedback loop measured in days.
#
# This target compiles the shipping server sources with a plain compiler and links them
# against a harness that drives the same event dispatcher a network connection drives.
# The transport is the only thing left out: GameNetworkingSockets needs protobuf, abseil
# and OpenSSL, none of which have anything to do with whether the server agrees with
# itself about who owns an actor. `stub/` supplies the headers those types appear in, and
# `harness/connect/ConnectStub.cpp` supplies the handful of `TiltedPhoques::Server`
# members the server calls (each a no-op recording what would have been sent).
#
#   make -C Tools/rfab/linux-server -j2          # build the harness
#   make -C Tools/rfab/linux-server -j2 objects  # compile the server only (fast gate)
#   make -C Tools/rfab/linux-server run          # build and run
#   make -C Tools/rfab/linux-server SANITIZE=1 run   # the same under ASan + UBSan
#
# Scope: Code/server + components + base + encoding + common + TiltedCore + lua.
# The Skyrim client (hooks, D3D11, CEF) is not part of this target and stays on xmake.

REPO_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
DEPS_DIR  ?= $(REPO_ROOT)/build/deps

# SANITIZE=1 builds the same server and harness with AddressSanitizer + UndefinedBehaviorSanitizer
# into their own object tree. A server that reads an entity it destroyed a tick earlier closes the
# session for every player; without the sanitizers such a read usually "works" in a test.
ifeq ($(SANITIZE),1)
  # -g1: line tables only, enough for file:line in a report at a fraction of the object size.
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g1
  BUILD_SUFFIX := -asan
  # Vendored sources keep AddressSanitizer but drop the UB checks (same reason as in
  # linux-core/Makefile: upstream code we cannot fix from here would fail every run).
  VENDOR_SAN := -fno-sanitize=undefined
  # The server allocates through mimalloc (TiltedCore's allocators), whose blocks AddressSanitizer
  # does not see on its own. MI_TRACK_ASAN makes mimalloc poison what it frees, so a use after free
  # of anything held in a TiltedPhoques container is reported like one of a std container.
  MIMALLOC_SAN := -DMI_TRACK_ASAN=1
endif

BUILD_DIR ?= $(REPO_ROOT)/build/linux-server$(BUILD_SUFFIX)
OBJ_DIR   := $(BUILD_DIR)/obj
BIN       := $(BUILD_DIR)/servertests

TILTEDCORE    := $(DEPS_DIR)/TiltedCore-0.2.9
TILTEDCONNECT := $(DEPS_DIR)/TiltedConnect
HOPSCOTCH     := $(DEPS_DIR)/hopscotch
GLM           := $(DEPS_DIR)/glm
ENTT          := $(DEPS_DIR)/entt
SPDLOG        := $(DEPS_DIR)/spdlog
SOL2          := $(DEPS_DIR)/sol2
LUA           := $(DEPS_DIR)/lua
HTTPLIB       := $(DEPS_DIR)/httplib
MIMALLOC      := $(DEPS_DIR)/mimalloc
ZLIB          := $(DEPS_DIR)/zlib

CXX  ?= g++
CC   ?= gcc
OPT  ?= -O0

INCLUDES := \
  -I$(REPO_ROOT)/Code \
  -I$(REPO_ROOT)/Code/common \
  -I$(REPO_ROOT)/Code/server \
  -I$(REPO_ROOT)/Code/encoding \
  -I$(REPO_ROOT)/Code/components \
  -I$(REPO_ROOT)/Code/components/console \
  -I$(REPO_ROOT)/Code/components/es_loader \
  -I$(REPO_ROOT)/Code/base \
  -I$(REPO_ROOT)/Libraries \
  -I$(REPO_ROOT)/Tools/rfab/linux-server/stub \
  -I$(REPO_ROOT)/Tools/rfab/linux-server/harness \
  -I$(DEPS_DIR)/tcinc \
  -I$(TILTEDCORE)/Code/core/include \
  -I$(TILTEDCONNECT)/Code/connect/include \
  -I$(ENTT)/src \
  -I$(SPDLOG)/include \
  -I$(GLM) \
  -I$(HOPSCOTCH)/include \
  -I$(SOL2)/include \
  -I$(HTTPLIB) \
  -I$(LUA) \
  -I$(MIMALLOC)/include \
  -I$(ZLIB)

# RFAB_NO_HTTPLIB_SSL: the harness has no OpenSSL and never announces a server to the
# public list, so the announcer compiles without TLS instead of dropping the file.
DEFINES := -DNOMINMAX -DRFAB_NO_HTTPLIB_SSL

CXXFLAGS += -std=c++20 $(OPT) -fPIC $(DEFINES) -w $(SAN_FLAGS)
DEPFLAGS := -MMD -MP
CFLAGS   += -std=c11 $(OPT) -fPIC $(SAN_FLAGS) $(VENDOR_SAN)

# The server is built with a precompiled header in the Windows pipeline and several headers
# rely on it (`PacketEvent.h` takes ConnectionId_t from TiltedConnect without including it).
SERVER_PCH    := -include $(REPO_ROOT)/Code/server/Pch.h
# Same story for the protocol: `Messages/Message.h` takes Serialization/String from
# TiltedCore without including it, because xmake gives every encoding TU EncodingPch.h.
ENCODING_PCH  := -include $(REPO_ROOT)/Code/encoding/EncodingPch.h
COMPONENT_PCH := $(SERVER_PCH) -include $(REPO_ROOT)/Tools/rfab/linux-server/stub/ComponentCompat.h

SERVER_SRCS    := $(shell find $(REPO_ROOT)/Code/server -name '*.cpp')
# `components/imgui` is the launcher/client's clipboard bridge and needs the Windows
# clipboard plus Dear ImGui; the server never includes it.
COMPONENT_SRCS := $(shell find $(REPO_ROOT)/Code/components -name '*.cpp' -not -name '*Test.cpp' \
                    -not -path '*/imgui/*' -not -name '*_Win32.cpp')
# `Code/base/tests` is gtest, which belongs to the Windows test target, not to the server.
BASE_SRCS      := $(shell find $(REPO_ROOT)/Code/base -name '*.cpp' -not -path '*/base/tests/*')
ENCODING_SRCS  := $(shell find $(REPO_ROOT)/Code/encoding -name '*.cpp')
COMMON_SRCS    := $(shell find $(REPO_ROOT)/Code/common -name '*.cpp')
CORE_SRCS      := $(wildcard $(TILTEDCORE)/Code/core/src/*.cpp)
LUA_SRCS       := $(filter-out %/lua.c %/luac.c %/onelua.c,$(wildcard $(LUA)/*.c))
# The es_loader reads compressed records, so zlib is part of the server's own code path.
ZLIB_SRCS      := $(wildcard $(ZLIB)/*.c)
HARNESS_SRCS   := $(wildcard $(REPO_ROOT)/Tools/rfab/linux-server/harness/*.cpp)
CONNECT_SRCS   := $(wildcard $(REPO_ROOT)/Tools/rfab/linux-server/harness/connect/*.cpp) \
                  $(TILTEDCONNECT)/Code/connect/src/Packet.cpp

rel_name = $(subst /,_,$(patsubst $(DEPS_DIR)/%,%,$(patsubst $(REPO_ROOT)/%,%,$(basename $(1)))))
obj_of = $(OBJ_DIR)/$(1)/$(call rel_name,$(2))

SERVER_OBJS    := $(foreach s,$(SERVER_SRCS),$(call obj_of,server,$(s)).o)
COMPONENT_OBJS := $(foreach s,$(COMPONENT_SRCS),$(call obj_of,components,$(s)).o)
BASE_OBJS      := $(foreach s,$(BASE_SRCS),$(call obj_of,base,$(s)).o)
ENCODING_OBJS  := $(foreach s,$(ENCODING_SRCS),$(call obj_of,encoding,$(s)).o)
COMMON_OBJS    := $(foreach s,$(COMMON_SRCS),$(call obj_of,common,$(s)).o)
CORE_OBJS      := $(foreach s,$(CORE_SRCS),$(call obj_of,core,$(s)).o)
LUA_OBJS       := $(foreach s,$(LUA_SRCS),$(call obj_of,lua,$(s)).o)
ZLIB_OBJS      := $(foreach s,$(ZLIB_SRCS),$(call obj_of,zlib,$(s)).o)
HARNESS_OBJS   := $(foreach s,$(HARNESS_SRCS),$(call obj_of,harness,$(s)).o)
CONNECT_OBJS   := $(foreach s,$(CONNECT_SRCS),$(call obj_of,connect,$(s)).o)
MIMALLOC_OBJ   := $(OBJ_DIR)/mimalloc.o

ALL_OBJS := $(SERVER_OBJS) $(COMPONENT_OBJS) $(BASE_OBJS) $(ENCODING_OBJS) \
            $(COMMON_OBJS) $(CORE_OBJS) $(LUA_OBJS) $(ZLIB_OBJS) $(HARNESS_OBJS) $(CONNECT_OBJS) $(MIMALLOC_OBJ)
ALL_DEPS := $(ALL_OBJS:.o=.d)

.PHONY: all objects run clean

all: $(BIN)

# The fast gate: the server must at least compile. No transport, no link, no game.
objects: $(SERVER_OBJS)
	@echo "  OK    server: $(words $(SERVER_OBJS)) translation units compiled"

run: $(BIN)
	@$(BIN)

$(BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD    $(notdir $@)"
	@$(CXX) $(SAN_FLAGS) $(ALL_OBJS) -o $@ -lpthread -ldl

define c_compile_rule
$(call obj_of,$(1),$(2)).o: $(2) $(FLAGS_STAMP)
	@mkdir -p $$(dir $$@)
	@echo "  CC    $$(patsubst $(REPO_ROOT)/%,%,$$<)"
	@$$(CC) $$(CFLAGS) -w -I$(ZLIB) -c $$< -o $$@
endef

# Objects rebuild when a *flag* changes, not whenever this file is edited. Depending on
# $(MAKEFILE_LIST) meant that adding one source line recompiled all 333 translation units,
# which is how a "fast" loop stops being run. The stamp is rewritten only when the
# compiler flags, defines or include paths actually differ, so a stale object is still
# impossible and adding a source is cheap.
FLAGS_SIG   := $(CXXFLAGS) $(CFLAGS) $(DEFINES) $(INCLUDES) $(OPT)
FLAGS_STAMP := $(BUILD_DIR)/flags.stamp

.PHONY: FORCE
FORCE:

$(FLAGS_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@printf '%s' '$(FLAGS_SIG)' | md5sum | cut -d' ' -f1 > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

define compile_rule
$(call obj_of,$(1),$(2)).o: $(2) $(FLAGS_STAMP)
	@mkdir -p $$(dir $$@)
	@echo "  CXX   $$(patsubst $(REPO_ROOT)/%,%,$$<)"
	@$$(CXX) $$(CXXFLAGS) $$(DEPFLAGS) $$(INCLUDES) $(3) -c $$< -o $$@
endef

$(foreach s,$(SERVER_SRCS),$(eval $(call compile_rule,server,$(s),$(SERVER_PCH))))
$(foreach s,$(COMPONENT_SRCS),$(eval $(call compile_rule,components,$(s),$(COMPONENT_PCH))))
$(foreach s,$(BASE_SRCS),$(eval $(call compile_rule,base,$(s),)))
$(foreach s,$(ENCODING_SRCS),$(eval $(call compile_rule,encoding,$(s),$(ENCODING_PCH))))
$(foreach s,$(COMMON_SRCS),$(eval $(call compile_rule,common,$(s),$(ENCODING_PCH))))
$(foreach s,$(CORE_SRCS),$(eval $(call compile_rule,core,$(s),$(VENDOR_SAN))))
$(foreach s,$(LUA_SRCS),$(eval $(call c_compile_rule,lua,$(s))))
$(foreach s,$(ZLIB_SRCS),$(eval $(call c_compile_rule,zlib,$(s))))
$(foreach s,$(HARNESS_SRCS),$(eval $(call compile_rule,harness,$(s),$(SERVER_PCH))))
$(foreach s,$(CONNECT_SRCS),$(eval $(call compile_rule,connect,$(s),$(SERVER_PCH))))

$(MIMALLOC_OBJ): $(MIMALLOC)/src/static.c $(MAKEFILE_LIST)
	@mkdir -p $(dir $@)
	@echo "  CC    mimalloc"
	@$(CC) $(CFLAGS) $(MIMALLOC_SAN) -w -UMI_MALLOC_OVERRIDE -I$(MIMALLOC)/include -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_DEPS)
