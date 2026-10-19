#ifndef VAM_MODEL_H
#define VAM_MODEL_H

/* Frontend-owned copy of a vam_page, with growable sections for paging. */

#include <stdbool.h>

#include "vam_backend.h"

typedef struct model_section {
    char title[VAM_TEXT_MAX];
    int layout;
    uint32_t total;
    uint32_t count, cap;
    vam_item *items;
} model_section;

typedef struct model_page {
    int kind;
    char uri[VAM_URI_MAX];
    char title[VAM_TEXT_MAX];
    char subtitle[VAM_TEXT_MAX];
    char description[VAM_DESC_MAX];
    char image_url[VAM_URL_MAX];
    bool is_saved;
    uint32_t section_count;
    model_section *sections;
} model_page;

model_page *model_from_vam(const vam_page *p);
void model_free(model_page *m);
/* Append the next slice of section 0. Ignores slices that don't line up. */
void model_append(model_page *m, const vam_page *p);
bool model_has_more(const model_page *m);
/* Update is_saved on every item / page matching uri. */
void model_set_saved(model_page *m, const char *uri, bool saved);

#endif
