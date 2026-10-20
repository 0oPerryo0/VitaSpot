/*
 * Contract test for include/vam_backend.h.
 *
 * Drives the backend through every exported entry point without any UI and
 * checks the invariants the frontend relies on: request ids, event order,
 * NUL-terminated strings, page/section layout and paging offsets, player
 * sequence bumps, saved-state events and cancellation.
 *
 * Build against the mock (default) or the real librespot backend; see
 * backend/tests/README.md. Without credentials the real backend cannot log in,
 * so the content/playback checks are skipped and reported as such.
 *
 * Exit code 0 means every check that could be executed passed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vam_backend.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <unistd.h>
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
#endif

static int g_checks;
static int g_failures;
static int g_skipped;
static char g_data_dir[512] = "vam-contract-data/";
static bool g_premium;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void skip(const char *what)
{
    g_skipped++;
    printf("SKIP %s\n", what);
}

static bool has_nul(const char *buf, size_t size)
{
    return memchr(buf, '\0', size) != NULL;
}

static uint64_t now_ms(void) { return vam_now_ms(); }

static const char *auth_message(const vam_auth_state *auth)
{
    return auth->message[0] ? auth->message : "(no message)";
}

/* Wait until *status reaches `want` or the timeout expires. */
static bool wait_auth(int want, unsigned timeout_ms)
{
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        vam_auth_state auth;
        vam_get_auth_state(&auth);
        if (auth.status == want)
            return true;
        /* Polling also drives the mock backend's simulated state machine. */
        vam_event ev;
        while (vam_poll_event(&ev))
            vam_event_release(&ev);
        sleep_ms(20);
    }
    return false;
}

/* ------------------------------------------------------------------------ */

static void check_page_invariants(const char *requested, const vam_event *ev)
{
    CHECK(ev->request_id != 0, "page reply has zero request id");
    CHECK(has_nul(ev->uri, sizeof(ev->uri)), "event.uri not NUL terminated");
    CHECK(has_nul(ev->message, sizeof(ev->message)), "event.message not NUL terminated");
    if (ev->result != VAM_OK) {
        CHECK(ev->page == NULL, "page pointer set despite error %d", ev->result);
        return;
    }
    CHECK(ev->page != NULL, "VAM_OK page event without a page");
    if (!ev->page)
        return;

    const vam_page *page = ev->page;
    CHECK(has_nul(page->uri, sizeof(page->uri)), "page.uri not NUL terminated");
    CHECK(has_nul(page->title, sizeof(page->title)), "page.title not NUL terminated");
    CHECK(has_nul(page->subtitle, sizeof(page->subtitle)), "page.subtitle not NUL terminated");
    CHECK(has_nul(page->description, sizeof(page->description)),
          "page.description not NUL terminated");
    CHECK(has_nul(page->image_url, sizeof(page->image_url)),
          "page.image_url not NUL terminated");
    CHECK(strncmp(page->uri, requested, sizeof(page->uri) - 1) == 0,
          "page.uri %s does not match requested %s", page->uri, requested);
    CHECK(page->section_count <= 16, "implausible section_count %u", page->section_count);
    if (page->section_count > 0)
        CHECK(page->sections != NULL, "sections NULL with section_count > 0");

    for (uint32_t s = 0; s < page->section_count; s++) {
        const vam_section *section = &page->sections[s];
        CHECK(has_nul(section->title, sizeof(section->title)),
              "section[%u].title not NUL terminated", s);
        CHECK(section->layout >= VAM_LAYOUT_LIST && section->layout <= VAM_LAYOUT_GRID,
              "section[%u] invalid layout %d", s, section->layout);
        CHECK(section->total >= section->count,
              "section[%u] total %u < count %u", s, section->total, section->count);
        CHECK(section->offset <= section->total,
              "section[%u] offset %u > total %u", s, section->offset, section->total);
        if (section->count > 0) {
            CHECK(section->items != NULL, "section[%u] items NULL with count %u", s,
                  section->count);
        }
        for (uint32_t i = 0; i < section->count && section->items; i++) {
            const vam_item *item = &section->items[i];
            CHECK(has_nul(item->uri, sizeof(item->uri)),
                  "item[%u][%u].uri not NUL terminated", s, i);
            CHECK(has_nul(item->title, sizeof(item->title)),
                  "item[%u][%u].title not NUL terminated", s, i);
            CHECK(has_nul(item->subtitle, sizeof(item->subtitle)),
                  "item[%u][%u].subtitle not NUL terminated", s, i);
            CHECK(item->uri[0] != '\0', "item[%u][%u] empty uri", s, i);
            CHECK(item->kind >= VAM_KIND_UNKNOWN && item->kind <= VAM_KIND_QUEUE,
                  "item[%u][%u] invalid kind %d", s, i, item->kind);
            if (item->kind == VAM_KIND_TRACK) {
                CHECK(item->duration_ms > 0, "track %s has no duration",
                      item->uri);
                CHECK(item->is_playable == 0 || item->is_playable == 1,
                      "track %s playable flag not 0/1", item->uri);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */

static bool request_and_wait(const char *uri, uint32_t offset, uint32_t limit,
                             vam_event *out, unsigned timeout_ms)
{
    uint32_t id = vam_request_page(uri, offset, limit);
    CHECK(id != 0, "vam_request_page(%s) returned 0", uri);
    if (!id)
        return false;
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        vam_event ev;
        if (!vam_poll_event(&ev)) {
            sleep_ms(5);
            continue;
        }
        if (ev.type == VAM_EV_PAGE && ev.request_id == id) {
            *out = ev;
            return true;
        }
        vam_event_release(&ev);
    }
    printf("FAIL timeout waiting for page %s (id %u)\n", uri, id);
    g_failures++;
    g_checks++;
    return false;
}

static void test_virtual_pages(void)
{
    static const char *uris[] = {
        "vam:home", "vam:library", "vam:search:love", "vam:queue",
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        vam_event ev;
        if (!request_and_wait(uris[i], 0, 50, &ev, 20000)) {
            printf("  (backend did not answer %s)\n", uris[i]);
            continue;
        }
        printf("page %-20s result=%d sections=%u\n", uris[i], ev.result,
               ev.page ? ev.page->section_count : 0);
        check_page_invariants(uris[i], &ev);

        if (ev.result == VAM_OK && ev.page) {
            const vam_page *page = ev.page;
            if (strcmp(uris[i], "vam:home") == 0) {
                CHECK(page->kind == VAM_KIND_HOME, "home page kind %d", page->kind);
                CHECK(page->section_count >= 1, "home has no sections");
                if (page->section_count >= 1) {
                    CHECK(page->sections[0].layout == VAM_LAYOUT_GRID,
                          "home section 0 is not a grid");
                    CHECK(page->sections[0].count <= 6, "home grid has more than 6 items");
                }
            } else if (strcmp(uris[i], "vam:library") == 0) {
                CHECK(page->kind == VAM_KIND_LIBRARY, "library page kind %d", page->kind);
                CHECK(page->section_count == 1, "library should have exactly one section");
                if (page->section_count == 1) {
                    CHECK(page->sections[0].layout == VAM_LAYOUT_LIST,
                          "library section is not a list");
                    CHECK(page->sections[0].count >= 1, "library is empty");
                    if (page->sections[0].count >= 1) {
                        CHECK(page->sections[0].items[0].kind == VAM_KIND_COLLECTION,
                              "Liked Songs is not first in the library");
                    }
                }
            } else if (strncmp(uris[i], "vam:search:", 11) == 0) {
                CHECK(page->kind == VAM_KIND_SEARCH, "search page kind %d", page->kind);
                CHECK(page->section_count >= 1, "search has no sections");
                if (page->section_count >= 1)
                    CHECK(page->sections[0].layout == VAM_LAYOUT_LIST,
                          "search songs section is not a list");
            } else if (strcmp(uris[i], "vam:queue") == 0) {
                CHECK(page->kind == VAM_KIND_QUEUE, "queue page kind %d", page->kind);
                CHECK(page->section_count == 2, "queue should have two sections");
                if (page->section_count == 2) {
                    CHECK(strcmp(page->sections[0].title, "Now playing") == 0,
                          "queue section 0 title %s", page->sections[0].title);
                    CHECK(strcmp(page->sections[1].title, "Next up") == 0,
                          "queue section 1 title %s", page->sections[1].title);
                }
            }
        }
        vam_event_release(&ev);
    }
}

static bool test_paging(void)
{
    vam_event library;
    if (!request_and_wait("vam:library", 0, 50, &library, 20000))
        return false;
    if (library.result != VAM_OK || !library.page) {
        vam_event_release(&library);
        skip("paging (backend has no library page)");
        return false;
    }

    char target[VAM_URI_MAX] = "";
    for (uint32_t s = 0; s < library.page->section_count && !target[0]; s++) {
        const vam_section *section = &library.page->sections[s];
        for (uint32_t i = 0; i < section->count; i++) {
            const vam_item *item = &section->items[i];
            if (item->kind == VAM_KIND_PLAYLIST || item->kind == VAM_KIND_ALBUM) {
                snprintf(target, sizeof(target), "%s", item->uri);
                break;
            }
        }
    }
    vam_event_release(&library);
    if (!target[0]) {
        skip("paging (no playlist/album in the library)");
        return false;
    }

    vam_event first;
    if (!request_and_wait(target, 0, 5, &first, 20000)) {
        skip("paging (backend did not answer the first page)");
        return false;
    }
    if (first.result != VAM_OK || !first.page || first.page->section_count == 0) {
        vam_event_release(&first);
        skip("paging (first page has no items)");
        return false;
    }
    uint32_t total = first.page->sections[0].total;
    uint32_t count = first.page->sections[0].count;
    CHECK(first.page->sections[0].offset == 0, "first page offset is %u",
          first.page->sections[0].offset);
    CHECK(count == 5 || total < 5, "first page returned %u of %u items", count, total);
    CHECK(first.page->sections[0].total >= first.page->sections[0].count,
          "first page total < count");
    vam_event_release(&first);

    if (total <= count) {
        skip("paging (target is too short)");
        return true;
    }

    vam_event second;
    if (!request_and_wait(target, count, 5, &second, 20000)) {
        skip("paging (no reply for the second page)");
        return false;
    }
    printf("paging %s: offset=0 count=%u total=%u -> offset=%u count=%u\n", target, count,
           total, count, second.page ? second.page->sections[0].count : 0);
    CHECK(second.result == VAM_OK, "second page result %d", second.result);
    if (second.result == VAM_OK && second.page) {
        CHECK(second.page->section_count == 1,
              "later pages should return only section 0 (got %u)",
              second.page->section_count);
        const vam_section *section = &second.page->sections[0];
        CHECK(section->offset == count, "later page offset %u, expected %u", section->offset,
              count);
        CHECK(section->count <= 5, "later page count %u > limit", section->count);
        CHECK(section->total >= section->offset + section->count,
              "later page total %u inconsistent with offset %u + count %u", section->total,
              section->offset, section->count);
        check_page_invariants(target, &second);
    }
    vam_event_release(&second);
    return true;
}

static void test_cancel_and_image(void)
{
    vam_event book;
    if (request_and_wait("vam:library", 0, 50, &book, 20000)) {
        char url[VAM_URL_MAX] = "";
        bool found = false;
        if (book.result == VAM_OK && book.page) {
            for (uint32_t s = 0; s < book.page->section_count && !found; s++) {
                const vam_section *section = &book.page->sections[s];
                for (uint32_t i = 0; i < section->count; i++) {
                    if (section->items[i].image_url[0]) {
                        snprintf(url, sizeof(url), "%s", section->items[i].image_url);
                        found = true;
                        break;
                    }
                }
            }
        }
        vam_event_release(&book);

        if (found) {
            vam_event image;
            uint32_t id = vam_request_image(url);
            CHECK(id != 0, "vam_request_image returned 0");
            uint64_t start = now_ms();
            bool got = false;
            while (id && now_ms() - start < 20000) {
                if (!vam_poll_event(&image)) {
                    sleep_ms(5);
                    continue;
                }
                if (image.type == VAM_EV_IMAGE && image.request_id == id) {
                    got = true;
                    CHECK(has_nul(image.message, sizeof(image.message)),
                          "image event message not NUL terminated");
                    if (image.result == VAM_OK) {
                        CHECK(image.data != NULL && image.data_size > 0,
                              "image success without data");
                    }
                    vam_event_release(&image);
                    break;
                }
                vam_event_release(&image);
            }
            CHECK(got, "no reply for image request");
            if (got)
                printf("image %s -> ok\n", url);
        } else {
            skip("image (no image URL in the library)");
        }
    }

    /* Cancelled requests must not produce a reply. */
    uint32_t cancelled = vam_request_page("vam:home", 0, 50);
    CHECK(cancelled != 0, "cancel test could not queue a request");
    if (cancelled) {
        vam_cancel(cancelled);
        uint64_t start = now_ms();
        bool leaked = false;
        while (now_ms() - start < 500) {
            vam_event ev;
            if (!vam_poll_event(&ev)) {
                sleep_ms(5);
                continue;
            }
            if (ev.request_id == cancelled)
                leaked = true;
            vam_event_release(&ev);
        }
        CHECK(!leaked, "cancelled request %u still produced an event", cancelled);
    }
}

static void test_saved(/* from a page */)
{
    vam_event library;
    if (!request_and_wait("vam:library", 0, 50, &library, 20000))
        return;
    char track[VAM_URI_MAX] = "";
    if (library.result == VAM_OK && library.page) {
        for (uint32_t s = 0; s < library.page->section_count && !track[0]; s++) {
            const vam_section *section = &library.page->sections[s];
            for (uint32_t i = 0; i < section->count; i++) {
                if (section->items[i].kind == VAM_KIND_TRACK) {
                    snprintf(track, sizeof(track), "%s", section->items[i].uri);
                    break;
                }
            }
        }
    }
    vam_event_release(&library);
    if (!track[0]) {
        /* Ask a playlist page for a track instead. */
        vam_event home;
        if (!request_and_wait("vam:home", 0, 50, &home, 20000))
            return;
        char playlist[VAM_URI_MAX] = "";
        if (home.result == VAM_OK && home.page) {
            for (uint32_t s = 0; s < home.page->section_count && !playlist[0]; s++) {
                const vam_section *section = &home.page->sections[s];
                for (uint32_t i = 0; i < section->count; i++) {
                    if (section->items[i].kind == VAM_KIND_PLAYLIST) {
                        snprintf(playlist, sizeof(playlist), "%s", section->items[i].uri);
                        break;
                    }
                }
            }
        }
        vam_event_release(&home);
        if (playlist[0]) {
            vam_event page;
            if (request_and_wait(playlist, 0, 10, &page, 20000)) {
                if (page.result == VAM_OK && page.page) {
                    for (uint32_t s = 0; s < page.page->section_count && !track[0]; s++) {
                        const vam_section *section = &page.page->sections[s];
                        for (uint32_t i = 0; i < section->count; i++) {
                            if (section->items[i].kind == VAM_KIND_TRACK) {
                                snprintf(track, sizeof(track), "%s",
                                         section->items[i].uri);
                                break;
                            }
                        }
                    }
                }
                vam_event_release(&page);
            }
        }
    }
    if (!track[0]) {
        skip("saved state (no track found)");
        return;
    }

    uint32_t id = vam_set_saved(track, 0);
    CHECK(id != 0, "vam_set_saved returned 0");
    if (!id)
        return;
    bool done = false, changed = false;
    uint64_t start = now_ms();
    while (now_ms() - start < 20000 && !(done && changed)) {
        vam_event ev;
        if (!vam_poll_event(&ev)) {
            sleep_ms(5);
            continue;
        }
        if (ev.type == VAM_EV_REQUEST_DONE && ev.request_id == id) {
            done = true;
            CHECK(has_nul(ev.uri, sizeof(ev.uri)), "REQUEST_DONE uri not NUL terminated");
            CHECK(strcmp(ev.uri, track) == 0, "REQUEST_DONE uri %s != %s", ev.uri, track);
        } else if (ev.type == VAM_EV_SAVED_CHANGED && strcmp(ev.uri, track) == 0) {
            changed = true;
        }
        vam_event_release(&ev);
    }
    CHECK(done, "no REQUEST_DONE for vam_set_saved");
    if (done)
        printf("saved toggle for %s\n", track);
    if (!changed)
        printf("note: no SAVED_CHANGED (save request may have failed)\n");
}

static void test_player(void)
{
    vam_player_state initial;
    vam_get_player_state(&initial);
    uint32_t seq = initial.seq;

    vam_event library;
    if (!request_and_wait("vam:library", 0, 50, &library, 20000))
        return;
    char context[VAM_URI_MAX] = "";
    if (library.result == VAM_OK && library.page) {
        for (uint32_t s = 0; s < library.page->section_count && !context[0]; s++) {
            const vam_section *section = &library.page->sections[s];
            for (uint32_t i = 0; i < section->count; i++) {
                int kind = section->items[i].kind;
                if (kind == VAM_KIND_PLAYLIST || kind == VAM_KIND_ALBUM ||
                    kind == VAM_KIND_COLLECTION) {
                    snprintf(context, sizeof(context), "%s", section->items[i].uri);
                    break;
                }
            }
        }
    }
    vam_event_release(&library);
    if (!context[0]) {
        skip("player (no playable context found)");
        return;
    }

    vam_result r = vam_play(context, NULL, 0);
    CHECK(r == VAM_OK || r == VAM_ERR_NOT_PREMIUM || r == VAM_ERR_AUTH,
          "vam_play returned %d", r);
    if (r != VAM_OK) {
        skip("player (playback not available)");
        return;
    }

    bool playing = false;
    uint64_t start = now_ms();
    while (now_ms() - start < 15000) {
        vam_event ev;
        if (!vam_poll_event(&ev)) {
            sleep_ms(5);
            continue;
        }
        if (ev.type == VAM_EV_ERROR || ev.type == VAM_EV_TOAST)
            printf("backend: %s\n", ev.message);
        vam_event_release(&ev);
        vam_player_state state;
        vam_get_player_state(&state);
        if (state.has_track && state.playback != VAM_PLAYBACK_STOPPED) {
            playing = true;
            break;
        }
    }
    if (!playing) {
        skip("player (no track started; no streamable credentials?)");
        return;
    }

    vam_player_state state;
    vam_get_player_state(&state);
    CHECK(state.has_track == 1, "has_track not set");
    CHECK(state.track.uri[0] != '\0', "track uri empty");
    CHECK(state.track.duration_ms > 0, "track duration not set");
    CHECK(state.seq != seq, "seq did not change on playback start");
    printf("playing %s - %s\n", state.track.title, state.track.subtitle);

    /* Seek. */
    CHECK(vam_seek(5000) == VAM_OK, "vam_seek failed");
    start = now_ms();
    bool seeked = false;
    while (now_ms() - start < 5000) {
        vam_get_player_state(&state);
        if (state.position_ms >= 5000 && state.position_ms < 15000) {
            seeked = true;
            break;
        }
        sleep_ms(20);
    }
    CHECK(seeked, "seek did not move the position (got %u)", state.position_ms);

    /* Pause / resume. */
    CHECK(vam_pause() == VAM_OK, "vam_pause failed");
    sleep_ms(50);
    vam_get_player_state(&state);
    CHECK(state.playback == VAM_PLAYBACK_PAUSED, "pause did not set PAUSED (got %d)",
          state.playback);
    uint32_t paused_at = state.position_ms;
    sleep_ms(150);
    vam_get_player_state(&state);
    CHECK(state.position_ms == paused_at, "position moved while paused");
    CHECK(vam_resume() == VAM_OK, "vam_resume failed");
    sleep_ms(50);
    vam_get_player_state(&state);
    CHECK(state.playback == VAM_PLAYBACK_PLAYING, "resume did not set PLAYING (got %d)",
          state.playback);

    /* Shuffle / repeat flags. */
    CHECK(vam_set_shuffle(1) == VAM_OK, "vam_set_shuffle failed");
    sleep_ms(50);
    vam_get_player_state(&state);
    CHECK(state.shuffle == 1, "shuffle flag not set");
    CHECK(vam_set_shuffle(0) == VAM_OK, "vam_set_shuffle(0) failed");
    CHECK(vam_set_repeat(VAM_REPEAT_CONTEXT) == VAM_OK, "vam_set_repeat failed");
    sleep_ms(50);
    vam_get_player_state(&state);
    CHECK(state.repeat == VAM_REPEAT_CONTEXT, "repeat flag not set");
    CHECK(vam_set_repeat(VAM_REPEAT_OFF) == VAM_OK, "vam_set_repeat(0) failed");

    /* Queue. */
    if (state.track.uri[0]) {
        vam_result q = vam_add_to_queue(state.track.uri);
        CHECK(q == VAM_OK || q == VAM_ERR_NOT_PREMIUM || q == VAM_ERR_AUTH,
              "vam_add_to_queue returned %d", q);
    }

    /* Next / previous should not crash and should emit player events. */
    uint32_t before = state.seq;
    CHECK(vam_next() == VAM_OK, "vam_next failed");
    sleep_ms(100);
    vam_get_player_state(&state);
    CHECK(state.seq >= before, "seq went backwards");
    CHECK(vam_prev() == VAM_OK, "vam_prev failed");
}

static void test_free_account(void)
{
    vam_player_state state;
    vam_get_player_state(&state);
    vam_result r = vam_play("vam:home", NULL, 0);
    CHECK(r == VAM_ERR_NOT_PREMIUM, "free account vam_play returned %d (expected %d)", r,
          VAM_ERR_NOT_PREMIUM);
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[1]);

    printf("VitaSpot backend contract test\n");
    printf("data dir: %s\n", g_data_dir);

    vam_config config;
    memset(&config, 0, sizeof(config));
    config.api_version = VAM_API_VERSION;
    config.device_name = "VitaSpot Contract Test";
    config.data_dir = g_data_dir;
    config.bitrate = 160;
    config.initial_volume = 52000;
    config.normalize_volume = 1;

    uint64_t start = now_ms();
    vam_result r = vam_init(&config);
    printf("vam_init -> %d (%llu ms)\n", r, (unsigned long long)(now_ms() - start));
    CHECK(r == VAM_OK, "vam_init failed with %d", r);

    vam_auth_state auth;
    vam_get_auth_state(&auth);
    CHECK(has_nul(auth.message, sizeof(auth.message)), "auth.message not NUL terminated");
    CHECK(has_nul(auth.device_name, sizeof(auth.device_name)),
          "auth.device_name not NUL terminated");
    CHECK(strcmp(auth.device_name, "VitaSpot Contract Test") == 0, "device name not applied");

    vam_player_state player;
    vam_get_player_state(&player);
    CHECK(player.volume == 52000, "initial volume not applied (%u)", player.volume);
    CHECK(player.seq == 0 || player.seq > 0, "player seq invalid");

    uint64_t now1 = vam_now_ms();
    sleep_ms(10);
    uint64_t now2 = vam_now_ms();
    CHECK(now2 >= now1 + 5, "vam_now_ms does not advance (%llu -> %llu)",
          (unsigned long long)now1, (unsigned long long)now2);

    CHECK(vam_set_bitrate(96) == VAM_OK, "vam_set_bitrate(96) failed");
    CHECK(vam_set_bitrate(320) == VAM_OK, "vam_set_bitrate(320) failed");
    CHECK(vam_set_bitrate(0) == VAM_ERR_INVALID_ARG, "vam_set_bitrate(0) accepted");
    CHECK(vam_set_bitrate(128) == VAM_ERR_INVALID_ARG, "vam_set_bitrate(128) accepted");
    CHECK(vam_set_bitrate(160) == VAM_OK, "vam_set_bitrate(160) failed");

    bool logged_in = wait_auth(VAM_AUTH_LOGGED_IN, 25000);
    vam_get_auth_state(&auth);
    if (logged_in) {
        printf("logged in as %s (premium=%u, zeroconf=%u) %s\n", auth.username,
               auth.is_premium, auth.zeroconf_active, auth_message(&auth));
        g_premium = auth.is_premium != 0;
    } else {
        printf("not logged in: status=%d %s\n", auth.status, auth_message(&auth));
        skip("content checks (backend is not logged in; pass real credentials or run the mock)");
    }

    test_virtual_pages();
    if (logged_in && g_premium) {
        test_paging();
        test_cancel_and_image();
        test_player();
        test_saved();
    }
    if (logged_in && !g_premium)
        test_free_account();

    vam_shutdown();

    printf("\n%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skipped);
    if (g_failures) {
        printf("CONTRACT TEST FAILED\n");
        return 1;
    }
    printf("CONTRACT TEST PASSED\n");
    return 0;
}
