#include <SDL.h>
#include <twogame.h>

int main(int argc, char** argv)
{
    Twogame tg(argv[0], "demo");
    tg.start();

    return 0;
}