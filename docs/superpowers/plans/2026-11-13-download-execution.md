# Downloader Download Execution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current scheduler `ENOTSUP` worker with a real HTTP/HTTPS download execution path that persists progress and atomically commits completed files.

**Architecture:** TaskManager owns active execution contexts and is the only layer that maps protocol callbacks to SQLite task state. Scheduler only runs a task worker callback. The existing ProtocolOps interface remains the protocol boundary; HTTP/libcurl reports progress and cooperative pause/cancel events through callbacks.

**Tech Stack:** ISO C99, CMake, clib mutex/memory/thread-pool, SQLite3, libcurl, POSIX local HTTP fixture.

**Spec:** `docs/superpowers/specs/2026-11-13-download-execution-design.md`

## Global Constraints

- Protocol modules must not access SQLite, IPC, or global task state.
- All downloads write to `.part` and use `filesystem_commit()` only after successful transfer.
- HTTP tests use a local loopback fixture and never depend on the public Internet.
- Downloader headers remain beside their `.c` files; no `include/` directory.
- Preserve the clib naming and formatting conventions in `AGENTS.md`.
- Existing Web and IPC operations must use the same TaskManager execution path.

## Review Focus

- A server that closes or interrupts a response must leave a resumable `.part` file and a failed task, not mark completion.
- A server that supports range requests must resume from the existing `.part` byte offset without duplicating bytes.
- Pause and delete racing with a worker must not leave an active protocol context or resurrect a deleted database row.
- A completed download whose target file already exists must fail closed without overwriting it.
- Scheduler shutdown must join active workers before TaskManager and database destruction.

### Task 1: Protocol event and HTTP control semantics

**Files:**
- Modify: `daemon/protocol.h`
- Modify: `protocols/http.c`
- Test: `tests/test-protocol.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ProtocolCallbacks.onCancelled` in addition to existing lifecycle callbacks.
- Consumes: existing `ProtocolOps` open/run/pause/resume/cancel/close table.

- [ ] Add a failing local-fixture test proving an HTTP transfer aborted by cancel reports cancellation rather than a generic failure.
- [ ] Extend `ProtocolCallbacks` with `onCancelled` and make HTTP distinguish pause, cancel, and transport failure after `curl_easy_perform()` returns.
- [ ] Ensure HTTP opens `.part` in append mode only when the persisted offset is nonzero and preserves the offset passed to libcurl.
- [ ] Run protocol tests and the complete existing test suite.

### Task 2: TaskManager execution context and worker lifecycle

**Files:**
- Modify: `daemon/task-manager.h`
- Modify: `daemon/task-manager.c`
- Modify: `daemon/scheduler.h`
- Modify: `daemon/scheduler.c`
- Modify: `daemon/main.c`
- Test: `tests/test-task-manager.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `int task_manager_execute (TaskManager*, DownloadTask*)` for the scheduler worker callback.
- Produces: `int task_manager_start()` that queues a heap-owned task snapshot and rejects duplicate active executions.
- Produces: callback-driven updates for started, progress, paused, cancelled, completed, and failed events.

- [ ] Add a failing test using a local protocol fixture or test protocol ops proving `task_manager_start()` creates one active execution and rejects a duplicate start.
- [ ] Define an active execution record containing the task snapshot, selected ProtocolOps, ProtocolTask context, and pause/cancel control state.
- [ ] Implement the scheduler worker bridge; ensure every heap-owned task snapshot is released on submit failure and worker completion.
- [ ] Implement TaskManager protocol callbacks that update SQLite under the manager mutex, including progress, speed, timestamps, and errors.
- [ ] On completion call `filesystem_commit()` before persisting `completed`; on commit failure persist `failed` and retain the `.part` file.
- [ ] Make pause/resume/cancel operate on the active protocol context when present, and make delete wait for or safely invalidate the active execution before removing the record/temp file.
- [ ] Reorder daemon shutdown so scheduler workers stop before TaskManager/database destruction.
- [ ] Run task-manager, scheduler, database, and complete tests.

### Task 3: Real HTTP local-fixture integration

**Files:**
- Create: `tests/http-fixture.c`
- Create: `tests/http-fixture.h`
- Modify: `tests/test-task-manager.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: controllable loopback HTTP fixture with normal, range, delayed, and interrupted response modes.
- Consumes: TaskManager execution API and the registered HTTP protocol adapter.

- [ ] Add a failing integration test for a complete download: exact output bytes, `completed` database state, correct progress, no `.part` file.
- [ ] Add a failing integration test for pre-existing `.part` resume with HTTP Range and exact final file contents.
- [ ] Add a failing integration test for pause/resume and interrupted transfer failure persistence.
- [ ] Implement the fixture with POSIX sockets and deterministic response bytes; do not use external commands or public services.
- [ ] Register the HTTP protocol in the test process, start a scheduler with the TaskManager worker bridge, and wait on database state with bounded polling.
- [ ] Run the focused integration tests and the complete test suite.

### Task 4: Web/IPC behavior and installation verification

**Files:**
- Modify: `daemon/main.c`
- Modify: `daemon/task-manager.c`
- Modify: `tests/test-web-e2e.c`
- Modify: `README.md`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: TaskManager start/pause/resume/delete/execute behavior from Tasks 1–3.
- Produces: Web and IPC starts trigger the same actual download execution and expose live persisted progress.

- [ ] Extend the Web e2e test to create and start a local-fixture task, poll until `completed`, and verify the committed file bytes.
- [ ] Extend IPC e2e coverage for create/start/pause/resume/delete using the same TaskManager.
- [ ] Verify scheduler stop precedes TaskManager/database cleanup and no worker remains after daemon exit.
- [ ] Verify installed static resources and default `0.0.0.0:8000` configuration remain intact.
- [ ] Run full build, full tests, local fixture integration, `git diff --check`, and a final read-only review.
