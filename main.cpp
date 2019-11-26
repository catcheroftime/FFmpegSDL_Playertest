#include <stdio.h>
#include <iostream>
#include "player.h"

#define SDL_MAIN_HANDLED

int main(int argc, char *args[])
{
    Player player;

    char filepath[] = "C:\\Users\\ubuntu\\Desktop\\xiamu.flv";
    player.play(filepath);

    return 0;
}




