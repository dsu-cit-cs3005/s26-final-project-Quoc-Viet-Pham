#include <cstdlib>
#include <ctime>
#include <iostream>

#include "Arena.h"

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>\n";
        return 1;
    }

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    Arena arena;
    if (!arena.load_config(argv[1])) {
        return 1;
    }

    arena.initialize_board();
    arena.place_obstacles();
    arena.load_robots("robots");
    arena.place_robots();
    arena.run_game();

    return 0;
}
