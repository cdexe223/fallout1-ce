#include "game/gmovie.h"

#include <stdio.h>
#include <string.h>

#include "game/cycle.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/moviefx.h"
#include "game/palette.h"
#include "int/movie.h"
#include "int/window.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"
#include "plib/gnw/touch.h"

namespace fallout {

#define GAME_MOVIE_WINDOW_WIDTH 640
#define GAME_MOVIE_WINDOW_HEIGHT 480

static char* gmovie_subtitle_func(char* movieFilePath);

// 0x5053FC
static const char* movie_list[MOVIE_COUNT] = {
    "iplogo.mve",
    "mplogo.mve",
    "intro.mve",
    "vexpld.mve",
    "cathexp.mve",
    "ovrintro.mve",
    "boil3.mve",
    "ovrrun.mve",
    "walkm.mve",
    "walkw.mve",
    "dipedv.mve",
    "boil1.mve",
    "boil2.mve",
    "raekills.mve",
};

// 0x596C78
static unsigned char gmovie_played_list[MOVIE_COUNT];

// gmovie_init
// 0x44E5C0
int gmovie_init()
{
    int volume = 0;
    if (gsound_background_is_enabled()) {
        volume = gsound_background_volume_get();
    }

    movieSetVolume(volume);

    movieSetSubtitleFunc(gmovie_subtitle_func);

    memset(gmovie_played_list, 0, sizeof(gmovie_played_list));

    return 0;
}

// 0x44E60C
void gmovie_reset()
{
    memset(gmovie_played_list, 0, sizeof(gmovie_played_list));
}

// 0x446064
void gmovie_exit()
{
}

// 0x44E638
int gmovie_load(DB_FILE* stream)
{
    if (db_fread(gmovie_played_list, sizeof(*gmovie_played_list), MOVIE_COUNT, stream) != MOVIE_COUNT) {
        return -1;
    }

    return 0;
}

// 0x44E664
int gmovie_save(DB_FILE* stream)
{
    if (db_fwrite(gmovie_played_list, sizeof(*gmovie_played_list), MOVIE_COUNT, stream) != MOVIE_COUNT) {
        return -1;
    }

    return 0;
}

// 0x44E690
int gmovie_play(int game_movie, int game_movie_flags)
{
    (void)game_movie;
    (void)game_movie_flags;
    return 0;
}

// 0x44EB04
bool gmovie_has_been_played(int movie)
{
    return gmovie_played_list[movie] == 1;
}

// 0x44EB1C
static char* gmovie_subtitle_func(char* movie_file_path)
{
    // 0x595226
    static char full_path[COMPAT_MAX_PATH];

    char* language;
    char* separator;

    config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY, &language);

    separator = strrchr(movie_file_path, '\\');
    if (separator != NULL) {
        movie_file_path = separator + 1;
    }

    snprintf(full_path, sizeof(full_path), "text\\%s\\cuts\\%s", language, movie_file_path);

    separator = strrchr(full_path, '.');
    if (*separator != '\0') {
        *separator = '\0';
    }

    strcpy(full_path + strlen(full_path), ".SVE");

    return full_path;
}

} // namespace fallout
