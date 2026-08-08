#include "app.h"
#include "bdo_format.h"
#include "midi_import.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static void try_open_file(MuseApp *app, const char *path) {
    size_t len = strlen(path);

    if (len > 4 && strcmp(path + len - 4, ".bdo") == 0) {
        if (bdo_load(path, app->char_name, &app->project) == 0) {
            /* strip the path, keep just the filename */
            const char *fname = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') fname = p + 1;
            snprintf(app->filename, sizeof(app->filename), "%.*s",
                     (int)(strlen(fname) - 4), fname);
            app->project.dirty = false;
            app->playhead_ms = 0;
            app->scroll_x = 0;
            /* figure out how many measures we need from the note data */
            double max_end = 0;
            for (int li = 0; li < app->project.num_layers; li++) {
                for (int si = 0; si < app->project.layers[li].num_sublayers; si++) {
                    NoteArray *na = &app->project.layers[li].sublayers[si];
                    for (int ni = 0; ni < na->count; ni++) {
                        double end = na->notes[ni].start + na->notes[ni].dur;
                        if (end > max_end) max_end = end;
                    }
                }
            }
            double beat_ms = 60000.0 / (app->project.bpm ? app->project.bpm : 120);
            int beats = (int)(max_end / beat_ms) + 4;
            int ts = app->project.time_sig ? app->project.time_sig : 4;
            app->measures = (beats + ts - 1) / ts;
            if (app->measures < 4) app->measures = 4;
            SDL_Log("Loaded: %s", path);
        } else {
            SDL_Log("Failed to load: %s", path);
        }
    } else if (len > 4 && (strcmp(path + len - 4, ".mid") == 0 ||
                            strcmp(path + len - 5, ".midi") == 0)) {
        if (midi_import(path, &app->project) == 0) {
            const char *fname = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') fname = p + 1;
            snprintf(app->filename, sizeof(app->filename), "%s", fname);
            size_t fl = strlen(app->filename);
            if (fl > 4 && strcmp(app->filename + fl - 4, ".mid") == 0)
                app->filename[fl - 4] = '\0';
            else if (fl > 5 && strcmp(app->filename + fl - 5, ".midi") == 0)
                app->filename[fl - 5] = '\0';
            app->project.dirty = true;
            app->playhead_ms = 0;
            app->scroll_x = 0;
            SDL_Log("Imported MIDI: %s", path);
        } else {
            SDL_Log("Failed to import: %s", path);
        }
    }
}

int main(int argc, char *argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    /* cd to wherever the exe lives so relative paths work no matter how we're launched */
    {
        const char *base = SDL_GetBasePath();
        if (base) {
            char *dir = SDL_strdup(base);
            /* chop trailing slash */
            size_t len = SDL_strlen(dir);
            if (len > 0 && (dir[len-1] == '/' || dir[len-1] == '\\'))
                dir[len-1] = '\0';
#ifdef _WIN32
            SetCurrentDirectoryA(dir);
#else
            chdir(dir);
#endif
            SDL_free(dir);
        }
    }

    MuseApp app;
    if (!muse_app_init(&app)) {
        SDL_Log("App init failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* open a file if one was passed on the command line */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            try_open_file(&app, argv[i]);
            break;
        }
    }

    while (app.running) {
        SDL_Event ev;
        if (!app.focused && !app.playing) {
            /* sleep when we're in the background - no point burning CPU */
            if (SDL_WaitEventTimeout(&ev, 250)) {
                muse_app_handle_event(&app, &ev);
                if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data)
                    try_open_file(&app, ev.drop.data);
            }
        }
        while (SDL_PollEvent(&ev)) {
            muse_app_handle_event(&app, &ev);

            /* drag-and-drop */
            if (ev.type == SDL_EVENT_DROP_FILE) {
                const char *file = ev.drop.data;
                if (file) {
                    try_open_file(&app, file);
                }
            }
        }

        muse_app_render(&app);

        /* vsync handles pacing when focused, throttle when backgrounded */
        if (!app.focused) {
            SDL_Delay(app.playing ? 32 : 100);
        }
    }

    muse_app_shutdown(&app);
    SDL_Quit();
    return 0;
}
