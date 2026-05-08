ROOT := ../..
BUILD_DIR := build
CXX ?= g++

LIBCO_SRCS := \
	$(ROOT)/co_epoll.cpp \
	$(ROOT)/co_cond.cpp \
	$(ROOT)/thread_worker.cpp \
	$(ROOT)/routine_context.cpp \
	$(ROOT)/co_routine.cpp \
	$(ROOT)/co_hook_sys_call.cpp \
	$(ROOT)/coctx.cpp \
	$(ROOT)/coctx_swap.S

COMMON_FLAGS := -I$(ROOT) -I. -g -fno-strict-aliasing -O1 --std=c++17 \
	-Wall -Werror -pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64
COMMON_LIBS := -lpthread -ldl

RISK_CHECK_PROGS := test_poll_semantics test_hook_syscall_semantics test_lifecycle_boundaries
RISK_DIAGNOSE_PROGS :=
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)

.PHONY: all check diagnose clean

all: $(addprefix $(BUILD_DIR)/,$(ALL_PROGS))

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

define RISK_PROGRAM_template
$(BUILD_DIR)/$(1): $(1).cpp risk_common.h $(LIBCO_SRCS) | $(BUILD_DIR)
	$(CXX) $(COMMON_FLAGS) $(SAN_FLAGS) -o $$@ $(1).cpp $(LIBCO_SRCS) $(COMMON_LIBS) $(SAN_FLAGS)
endef

$(foreach prog,$(ALL_PROGS),$(eval $(call RISK_PROGRAM_template,$(prog))))

check: $(addprefix $(BUILD_DIR)/,$(RISK_CHECK_PROGS))
	@if [ -z "$(RISK_CHECK_PROGS)" ]; then \
		echo "No risk-check programs configured"; \
	else \
		status=0; \
		for prog in $(RISK_CHECK_PROGS); do \
			echo "RUN risk-check $$prog"; \
			$(BUILD_DIR)/$$prog || status=1; \
		done; \
		exit $$status; \
	fi

diagnose: $(addprefix $(BUILD_DIR)/,$(RISK_DIAGNOSE_PROGS))
	@if [ -z "$(RISK_DIAGNOSE_PROGS)" ]; then \
		echo "No risk-diagnose programs configured"; \
	else \
		status=0; \
		for prog in $(RISK_DIAGNOSE_PROGS); do \
			echo "RUN risk-diagnose $$prog"; \
			$(BUILD_DIR)/$$prog || status=1; \
		done; \
		exit $$status; \
	fi

clean:
	rm -rf $(BUILD_DIR)
