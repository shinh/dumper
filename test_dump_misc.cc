#include "dump.h"

#include <stdio.h>

#include <SDL.h>

typedef struct {
    int b1 : 1;
    int b2 : 1;
    int b3 : 1;
    int b4 : 2;
} bitfields;

int main(int argc, char* argv[]) {
    dump_open(argv[0]);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Surface* s = SDL_SetVideoMode(640, 480, 32, SDL_SWSURFACE);
    printf("%p flags=%d, w=%d, h=%d, pitch=%d\n",
           s, s->flags, s->w, s->h, s->pitch);
    p(s);
    p(s->format);
    pv(SDL_GetVideoSurface());

    int i = 1;
    int *ip = &i;
    int **ipp = &ip;
    p(i);
    p(ip);
    p(ipp);

    {
        void* i;
        i = ip;
        p(i);
    }

    // bothersome chain.
    FILE* fp = fopen("test_dump.cc", "r");
    p(fp);

/* bit field is unimplemented
    bitfields bf = { 1, 0, 2, 2 };
    p(bf);
*/
}
