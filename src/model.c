#include "model.h"

#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, const char *src, size_t n)
{
    size_t len = strnlen(src, n - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
}

static bool reserve(model_section *s, uint32_t want)
{
    if (want <= s->cap)
        return true;
    uint32_t cap = s->cap ? s->cap : 32;
    while (cap < want)
        cap *= 2;
    vam_item *n = realloc(s->items, sizeof(vam_item) * cap);
    if (!n)
        return false;
    s->items = n;
    s->cap = cap;
    return true;
}

model_page *model_from_vam(const vam_page *p)
{
    model_page *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->kind = p->kind;
    copy_str(m->uri, p->uri, sizeof(m->uri));
    copy_str(m->title, p->title, sizeof(m->title));
    copy_str(m->subtitle, p->subtitle, sizeof(m->subtitle));
    copy_str(m->description, p->description, sizeof(m->description));
    copy_str(m->image_url, p->image_url, sizeof(m->image_url));
    m->is_saved = p->is_saved != 0;
    if (p->section_count && p->sections) {
        m->sections = calloc(p->section_count, sizeof(model_section));
        if (!m->sections) {
            free(m);
            return NULL;
        }
        m->section_count = p->section_count;
        for (uint32_t i = 0; i < p->section_count; i++) {
            const vam_section *src = &p->sections[i];
            model_section *dst = &m->sections[i];
            copy_str(dst->title, src->title, sizeof(dst->title));
            dst->layout = src->layout;
            dst->total = src->total > src->count ? src->total : src->count;
            if (src->count && src->items && reserve(dst, src->count)) {
                memcpy(dst->items, src->items, sizeof(vam_item) * src->count);
                dst->count = src->count;
            }
        }
    }
    return m;
}

void model_free(model_page *m)
{
    if (!m)
        return;
    for (uint32_t i = 0; i < m->section_count; i++)
        free(m->sections[i].items);
    free(m->sections);
    free(m);
}

void model_append(model_page *m, const vam_page *p)
{
    if (!m || !m->section_count || !p->section_count || !p->sections)
        return;
    const vam_section *src = &p->sections[0];
    model_section *dst = &m->sections[0];
    if (src->offset != dst->count || !src->count || !src->items)
        return;
    if (!reserve(dst, dst->count + src->count))
        return;
    memcpy(dst->items + dst->count, src->items, sizeof(vam_item) * src->count);
    dst->count += src->count;
    if (src->total > dst->total)
        dst->total = src->total;
    if (dst->count > dst->total)
        dst->total = dst->count;
}

bool model_has_more(const model_page *m)
{
    return m && m->section_count && m->sections[0].count < m->sections[0].total;
}

void model_set_saved(model_page *m, const char *uri, bool saved)
{
    if (!m)
        return;
    if (strcmp(m->uri, uri) == 0)
        m->is_saved = saved;
    for (uint32_t i = 0; i < m->section_count; i++)
        for (uint32_t j = 0; j < m->sections[i].count; j++)
            if (strcmp(m->sections[i].items[j].uri, uri) == 0)
                m->sections[i].items[j].is_saved = saved;
}
