# esl_proxy build
#
# Usage:
#   make          # default: qwen3_dynamic_manual_scope.h, QWEN3_SPMD_TIER=2
#   make run
#   make run WORKER_LOG=1
#
# Cases (cases/*.h):
#   make CASE=qwen3_dynamic_manual_scope.h run
#   make CASE=qwen3_dynamic_tensormap.h run
#   make CASE=paged_attention_unroll.h run
#   make CASE=paged_attention_unroll_manual_scope.h run
#
# Qwen3 SPMD tier (0=non-spmd .. 4=all-spmd; default 2 = spmd-4 for qwen3 cases):
#   make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=0 run
#
# Other compile / run flags:
#   make MAIN_LOG=0 CASE=paged_attention_unroll.h run
#   make clean
#
# CASE selects which cases/*.h orchestration header main.c compiles in.

ALL_CASES := \
	qwen3_dynamic_manual_scope.h \
	qwen3_dynamic_tensormap.h \
	qwen3_dynamic_tensormap_ot1.h \
	paged_attention_unroll.h \
	paged_attention_unroll_manual_scope.h \
	ed_a10_probe.h \
	ed_a11_probe.h \
	ed_a12_ring_stress.h

CASE ?= qwen3_dynamic_manual_scope.h

ifneq ($(filter $(CASE),$(ALL_CASES)),$(CASE))
$(error Unknown CASE='$(CASE)'. Valid: $(ALL_CASES))
endif

QWEN3_CASES := qwen3_dynamic_manual_scope.h qwen3_dynamic_tensormap.h qwen3_dynamic_tensormap_ot1.h

CC       := cc
# -MMD -MP emits a .d alongside each .o listing the headers it included, so a
# change to any include/algorithm/*.h (e.g. tensormap.h) rebuilds the dependent objects.
CFLAGS   := -g -std=c11 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=199309L \
	-I include/algorithm -I cases -DORCH_CASE=$(CASE) -MMD -MP -Wno-error=implicit-function-declaration

# -latomic is needed on Linux but not on macOS (clang)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS  := -lpthread -latomic
else
    LDFLAGS  := -lpthread
endif

# All four cases use struct Tensor / tensormap helpers.
# Qwen3 only: default spmd-4 tier; override via make QWEN3_SPMD_TIER=N.
ifneq ($(filter $(CASE),$(QWEN3_CASES)),)
QWEN3_SPMD_TIER ?= 2
CFLAGS   += -DQWEN3_SPMD_TIER=$(QWEN3_SPMD_TIER)
endif

ifneq ($(MAIN_LOG),)
CFLAGS   += -DMAIN_LOG=$(MAIN_LOG)
endif

ifneq ($(EXTRA_CFLAGS),)
CFLAGS   += $(EXTRA_CFLAGS)
endif

# Early-dispatch compile switch (Step 1+)
ED_ENABLE ?= 1
ED_UNFIN_THRESHOLD ?= 0xFFFF
EXEC_DURATION_SCALE ?= 10000
CFLAGS   += -DED_ENABLE=$(ED_ENABLE) -DED_UNFIN_THRESHOLD=$(ED_UNFIN_THRESHOLD) \
	-DEXEC_DURATION_SCALE=$(EXEC_DURATION_SCALE)

OBJ_DIR := build/obj
BIN_DIR := bin
TARGET  := $(BIN_DIR)/esl_proxy

SRCS := \
	src/main.c \
	src/algorithm/executor.c \
	src/algorithm/dispatch.c \
	src/algorithm/cutter.c \
	src/algorithm/manager.c \
	src/algorithm/log.c \
	src/algorithm/shm.c \
	src/algorithm/early_dispatch.c

OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Rebuild orchestration when case or orch-related flags change.
ORCH_STAMP := $(OBJ_DIR)/.orch-stamp
ORCH_CONFIG := case=$(CASE) tier=$(QWEN3_SPMD_TIER) log=$(MAIN_LOG) ed=$(ED_ENABLE) ed_thr=$(ED_UNFIN_THRESHOLD) dur_scale=$(EXEC_DURATION_SCALE)

.PHONY: all run clean force test-tensormap test-ed-record test-step2 test-step3 test-step4 test-step5 test-step6 test-step7 test-cutter-window compile_commands

compile_commands:
	python3 ./tools/gen_compile_commands.py

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rebuild when orchestration / ed config changes.
# ORCH_CONFIG 变化时（CASE / TIER / MAIN_LOG / ED_ENABLE / ED_UNFIN_THRESHOLD 任一切换），
# stamp 内容会改，所有 $(OBJS) 都必须重编——它们都直接或间接依赖这些宏（例如 dispatch.c
# 与 cutter.c 都在 ED_ENABLE 下条件编译 send_task CAS / 右开窗口路径）。历史上只挂
# main.o / early_dispatch.o / executor.o 三个对象，会在无 clean 的 ED 切换中复用旧
# dispatch.o，导致 test-step3 全部失败，这里改成统一挂到 $(OBJS)。
$(OBJS): $(ORCH_STAMP)
$(ORCH_STAMP): force | $(OBJ_DIR)
	@echo '$(ORCH_CONFIG)' | cmp -s - $@ 2>/dev/null || echo '$(ORCH_CONFIG)' > $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@


run: all
	WORKER_LOG=$(WORKER_LOG) $(TARGET)

test-tensormap: $(BIN_DIR)/test_tensormap
	$(BIN_DIR)/test_tensormap

TEST_TENSORMAP_CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -I include/algorithm

$(BIN_DIR)/test_tensormap: tests/test_tensormap.c | $(BIN_DIR)
	$(CC) $(TEST_TENSORMAP_CFLAGS) tests/test_tensormap.c -o $@ $(LDFLAGS)

test-ed-record: $(BIN_DIR)/test_ed_record
	$(BIN_DIR)/test_ed_record

$(BIN_DIR)/test_ed_record: tests/test_ed_record.c $(OBJ_DIR)/algorithm/early_dispatch.o $(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_ed_record.c $(OBJ_DIR)/algorithm/early_dispatch.o $(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

test-step2: $(BIN_DIR)/test_step2_protocol
	$(BIN_DIR)/test_step2_protocol

$(BIN_DIR)/test_step2_protocol: tests/test_step2_protocol.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step2_protocol.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# Step 3 send_task CAS 协议回归：需要 dispatch/executor 完整链路 + shm/ed 全局符号 + log。
# 该测试文件自带 g_state_buf stub（详见 tests/test_step3_next_block_idx.c），
# 因此这里不联 cutter.o，避免 g_state_buf 多重定义。
test-step3: $(BIN_DIR)/test_step3_next_block_idx
	$(BIN_DIR)/test_step3_next_block_idx

$(BIN_DIR)/test_step3_next_block_idx: tests/test_step3_next_block_idx.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step3_next_block_idx.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# Step 4 Hook 0 + late-arrival 线性化回归：
# 直接驱动 cutter::add_successors 与 propagate_dispatch_fanin，验证 fanin 贡献恰好一次。
test-step4: $(BIN_DIR)/test_step4_hook0_late_arrival
	$(BIN_DIR)/test_step4_hook0_late_arrival

$(BIN_DIR)/test_step4_hook0_late_arrival: tests/test_step4_hook0_late_arrival.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step4_hook0_late_arrival.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# Step 5 Hook 2 回归：resolve_dep 在 unfin 1->0 时切 DISPATCHED 并统一 push ready_queue。
test-step5: $(BIN_DIR)/test_step5_hook2_resolve_dep
	$(BIN_DIR)/test_step5_hook2_resolve_dep

$(BIN_DIR)/test_step5_hook2_resolve_dep: tests/test_step5_hook2_resolve_dep.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step5_hook2_resolve_dep.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# Step 6 Hook 1 + notify_once 回归：验证 stage->GATED、Hook2 释放、唯一 doorbell 写入。
test-step6: $(BIN_DIR)/test_step6_hook1_notify_once
	$(BIN_DIR)/test_step6_hook1_notify_once

$(BIN_DIR)/test_step6_hook1_notify_once: tests/test_step6_hook1_notify_once.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step6_hook1_notify_once.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# Step 7 race + ABA 回归：覆盖 Hook1/Hook2 两种通知先后，以及 ring 复用 stale notify 防护。
test-step7: $(BIN_DIR)/test_step7_notify_race_aba
	$(BIN_DIR)/test_step7_notify_race_aba

$(BIN_DIR)/test_step7_notify_race_aba: tests/test_step7_notify_race_aba.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step7_notify_race_aba.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

# cutter 提交窗口右开区间回归：只需 cutter/shm/log + ED（ED 提供未来 stager 相关符号占位，
# cutter.c 自身当前未直接引用，但链接进来对结果无影响并保持与其他 test-* 目标一致）。
test-cutter-window: $(BIN_DIR)/test_cutter_commit_window
	$(BIN_DIR)/test_cutter_commit_window

$(BIN_DIR)/test_cutter_commit_window: tests/test_cutter_commit_window.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_cutter_commit_window.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o -o $@ $(LDFLAGS)

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
