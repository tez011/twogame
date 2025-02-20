#include <SDL.h>
#include <spdlog/spdlog.h>
#include <twogame.h>

int main(int argc, char** argv)
{
    spdlog::set_pattern("[%E.%e %L %t] %^%v%$");

    Twogame tg(argv[0], "demo", argc >= 2 ? argv[1] : nullptr);
    tg.start();

    return 0;
}
