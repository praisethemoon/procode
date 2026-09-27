/* lap - fine-grained edit recorder for AI agents.
 * Common definitions shared by every translation unit.
 */
#ifndef LAP_H
#define LAP_H

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAP_VERSION "0.1.0"

#define LAP_DIR ".lap"
#define LAP_LOG_NAME "log.jsonl" /* the single-file log before chunks */
#define LAP_LOG_DIR "log"         /* .lap/log/: the history's chunk files */
#define LAP_MAIN_LINEAGE "main"   /* the first folder's line of history */
#define LAP_LINEAGE_NAME "lineage" /* .lap/lineage: a branch folder's id */
#define LAP_PARENT_NAME "parent"   /* .lap/parent: its parent folder's path */
/* A chunk is sealed once an append would take it past this many bytes. A
 * constant, so every folder chunks alike; LAP_TEST_CHUNK_BYTES overrides it
 * for tests only. */
#define LAP_CHUNK_BYTES (4u * 1024u * 1024u)
#define LAP_STATE_NAME "state.json"
#define LAP_SHADOW_NAME "shadow"
#define LAP_LOCK_NAME "lock"
#define LAP_IGNORE_NAME ".lapignore"

#define LAP_PATH_MAX 4096
#define LAP_MAX_FILE_SIZE (64u * 1024u * 1024u) /* refuse files larger than 64 MB */

/* Exit codes: 0 success, 1 user/repo error, 2 internal error (OOM, I/O corruption). */
#define LAP_EXIT_OK 0
#define LAP_EXIT_ERR 1
#define LAP_EXIT_FATAL 2

#endif /* LAP_H */
