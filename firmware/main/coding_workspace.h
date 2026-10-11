#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include "coding_flow.h"
#define CODING_RENDER_CAPACITY (CODING_STATUS_MAX + CODING_DETAIL_MAX + 16)
#define CODING_CATALOG_CAPACITY 8

typedef struct { char id[8]; char title[192]; } coding_catalog_item_t;
typedef struct {
 char detail[CODING_RENDER_CAPACITY];
 char render_detail[CODING_RENDER_CAPACITY];
 coding_catalog_item_t catalog[CODING_CATALOG_CAPACITY];
} coding_workspace_t;

static inline bool coding_workspace_acquire(coding_workspace_t **workspace) {
 if (*workspace) return true;
 *workspace = (coding_workspace_t *)calloc(1, sizeof(coding_workspace_t));
 return *workspace != NULL;
}
static inline void coding_workspace_release(coding_workspace_t **workspace) {
 free(*workspace);
 *workspace = NULL;
}
