/*
 *  BSD 2-Clause License
 *
 *  Copyright (c) 2025, ozkl
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <SDL.h>
#include <SDL_ttf.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ozterm.h"

#define COLS 80
#define ROWS 25
#define FONT_SIZE 16

static int g_font_width = 0;
static int g_font_height = 0;
static SDL_Texture* g_glyph_cache[128];
static SDL_Texture* g_glyph_cursor = NULL;

static int g_refresh_screen = 0;
static int g_master_fd = -1;

void measure_glyph_size(TTF_Font* font)
{
    TTF_SizeText(font, "M", &g_font_width, &g_font_height);  // "M" is usually the widest monospaced char
}

void build_g_glyph_cache(SDL_Renderer* renderer, TTF_Font* font, SDL_Color fg)
{
    for (int i = 32; i < 127; ++i)
    {
        char ch[2] = {i, 0};
        SDL_Surface* s = TTF_RenderText_Blended(font, ch, fg);
        g_glyph_cache[i] = SDL_CreateTextureFromSurface(renderer, s);
        SDL_FreeSurface(s);
    }

    g_glyph_cursor = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, g_font_width, g_font_height);
    if (!g_glyph_cursor)
    {
        fprintf(stderr, "Failed to create cursor texture: %s\n", SDL_GetError());
        return;
    }
    // Set target to the texture and draw to it
    SDL_SetRenderTarget(renderer, g_glyph_cursor);
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red
    SDL_RenderClear(renderer);
    // Reset render target to default
    SDL_SetRenderTarget(renderer, NULL);
}

void render_screen(SDL_Renderer* renderer, TTF_Font* font, Ozterm* terminal) {

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    for (int y = 0; y < terminal->row_count; ++y)
    {
        for (int x = 0; x < terminal->column_count; ++x)
        {
            SDL_Rect dst = {x * g_font_width, y * g_font_height, g_font_width, g_font_height};
            OztermCell * video = terminal->screen_active->buffer + (y * terminal->column_count + x);
            char ch = video->character;
            if (ch >= 32 && ch < 127)
                SDL_RenderCopy(renderer, g_glyph_cache[(int)ch], NULL, &dst);
        }
    }

    SDL_Rect cursor_dst = {terminal->screen_active->cursor_column * g_font_width, terminal->screen_active->cursor_row * g_font_height, g_font_width, g_font_height};
    SDL_RenderCopy(renderer, g_glyph_cursor, NULL, &cursor_dst);
    
    SDL_RenderPresent(renderer);
}

static void write_to_master(Ozterm* terminal, const uint8_t* data, int32_t size)
{
    if (g_master_fd >= 0)
    {
        write(g_master_fd, data, size);
    }
}

static void terminal_refresh(Ozterm* terminal)
{
    g_refresh_screen = 1;
}

static void terminal_set_character(Ozterm* terminal, int16_t row, int16_t column, uint8_t character)
{
    //TODO: optimize by only updating individual characters not full screen
    g_refresh_screen = 1;
}

static void terminal_move_cursor(Ozterm* terminal, int16_t old_row, int16_t old_column, int16_t row, int16_t column)
{
    //TODO: optimize by only updating cursor not full screen
    g_refresh_screen = 1;
}

static void update_pty_winsize(int master_fd, int cols, int rows)
{
    struct winsize ws =
    {
        .ws_col = cols,
        .ws_row = rows,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    ioctl(master_fd, TIOCSWINSZ, &ws);
}


int main()
{
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    
    TTF_Font* font = TTF_OpenFont("fonts/DejaVuSansMono.ttf", FONT_SIZE);
    if (!font)
    {
        fprintf(stderr, "Failed to load font: %s\n", TTF_GetError());
        exit(1);
    }

    measure_glyph_size(font);

    SDL_Window* win = SDL_CreateWindow("Ozterm", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, COLS * g_font_width, ROWS * g_font_height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    pid_t pid = forkpty(&g_master_fd, NULL, NULL, NULL);

    if (pid > 0)
    {
        update_pty_winsize(g_master_fd, COLS, ROWS);
    }
    else if (pid == 0)
    {
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/bash", "bash", NULL);
        perror("execl");
        exit(1);
    }

    // Init screen with white on black
    SDL_Color white = {255, 255, 255, 255};

    int running = 1;
    char buf[8192];

    build_g_glyph_cache(renderer, font, white);

    Ozterm * terminal = ozterm_create(ROWS, COLS);
    terminal->write_to_master_function = write_to_master;
    terminal->refresh_function = terminal_refresh;
    terminal->set_character_function = terminal_set_character;
    terminal->move_cursor_function = terminal_move_cursor;

    while (running)
    {
        g_refresh_screen = 0;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_master_fd, &fds);
        struct timeval tv = {0, 10000}; // 10ms
        select(g_master_fd + 1, &fds, NULL, NULL, &tv);

        if (FD_ISSET(g_master_fd, &fds))
        {
            int len = read(g_master_fd, buf, sizeof(buf));
            if (len >= 0)
            {
                ozterm_have_read_from_master(terminal, (uint8_t*)buf, len);
            }
        }

        SDL_Event e;
        SDL_WaitEventTimeout(&e, 15);
        if (e.type == SDL_QUIT)
        {
            running = 0;
        }
        else if (e.type == SDL_KEYDOWN)
        {
            uint8_t terminal_key = OZTERM_KEY_NONE;

            SDL_Keycode sdl_key = e.key.keysym.sym;

            SDL_Keymod mod = SDL_GetModState();
            uint8_t modifier = 0;

            if (mod & KMOD_LSHIFT) modifier |= OZTERM_KEYM_LEFTSHIFT;
            if (mod & KMOD_RSHIFT) modifier |= OZTERM_KEYM_RIGHTSHIFT;
            if (mod & KMOD_CTRL)   modifier |= OZTERM_KEYM_CTRL;
            if (mod & KMOD_ALT)    modifier |= OZTERM_KEYM_ALT;

            switch (sdl_key)
            {
                case SDLK_RETURN:   terminal_key = OZTERM_KEY_RETURN; break;
                case SDLK_BACKSPACE:terminal_key = OZTERM_KEY_BACKSPACE; break;
                case SDLK_ESCAPE:   terminal_key = OZTERM_KEY_ESCAPE; break;
                case SDLK_TAB:      terminal_key = OZTERM_KEY_TAB; break;
                case SDLK_DOWN:     terminal_key = OZTERM_KEY_DOWN; break;
                case SDLK_UP:       terminal_key = OZTERM_KEY_UP; break;
                case SDLK_LEFT:     terminal_key = OZTERM_KEY_LEFT; break;
                case SDLK_RIGHT:    terminal_key = OZTERM_KEY_RIGHT; break;
                case SDLK_HOME:     terminal_key = OZTERM_KEY_HOME; break;
                case SDLK_END:      terminal_key = OZTERM_KEY_END; break;
                case SDLK_PAGEUP:   terminal_key = OZTERM_KEY_PAGEUP; break;
                case SDLK_PAGEDOWN: terminal_key = OZTERM_KEY_PAGEDOWN; break;
                case SDLK_F1:  terminal_key = OZTERM_KEY_F1; break;
                case SDLK_F2:  terminal_key = OZTERM_KEY_F2; break;
                case SDLK_F3:  terminal_key = OZTERM_KEY_F3; break;
                case SDLK_F4:  terminal_key = OZTERM_KEY_F4; break;
                case SDLK_F5:  terminal_key = OZTERM_KEY_F5; break;
                case SDLK_F6:  terminal_key = OZTERM_KEY_F6; break;
                case SDLK_F7:  terminal_key = OZTERM_KEY_F7; break;
                case SDLK_F8:  terminal_key = OZTERM_KEY_F8; break;
                case SDLK_F9:  terminal_key = OZTERM_KEY_F9; break;
                case SDLK_F10: terminal_key = OZTERM_KEY_F10; break;
                case SDLK_F11: terminal_key = OZTERM_KEY_F11; break;
                case SDLK_F12: terminal_key = OZTERM_KEY_F12; break;
                
                default:
                    //we do not send character keys here, it is SDL_TEXTINPUT's job
                    break;
            }

            if (terminal_key == OZTERM_KEY_NONE)
            {
                if (modifier & OZTERM_KEYM_CTRL)
                {
                    if (sdl_key >= 0 && sdl_key < 128)
                    {
                        terminal_key = sdl_key;
                    }
                }
            }

            if (terminal_key != OZTERM_KEY_NONE)
            {
                ozterm_send_key(terminal, modifier, terminal_key);
            }
        }
        else if (e.type == SDL_TEXTINPUT)
        {
            if (!(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT)))
            {
                ozterm_send_key(terminal, OZTERM_KEYM_NONE, e.text.text[0]);
            }
        }
        
        if (g_refresh_screen)
        {
            render_screen(renderer, font, terminal);
        }
    }

    close(g_master_fd);
    SDL_Quit();
    return 0;
}
