#pragma once

#include<vector>
#include<string>
#include"RobotBase.h"
#include"RadarObj.h"

// Stores all settings loaded from config.txt
struct Config{
    int height = 20;
    int width = 20;
    int max_rounds = 10000;
    double sleep_interval = 0.5;
    bool game_state_live = true;
    int flamethrowers = 5;
    int pits = 5;
    int mounts = 5;
};

// What kind of terrain is permanently in a board cell
enum class Terrian{
    Empty,
    Mound,
    Pit,
    Flamethrower
};

// One square on the arena board
struct cell{
    Terrian terrian = Terrian::Empty;
    int robot_index = -1;
};

// Stores one loaded robot plus extra data Arena needs
struct RobotEntry{
    RobotBase* robot = nullptr;
    void* handle = nullptr; // shared library handle from dlopen
    char symbol = '?';  // unique character shown on board
    bool alive = true;  // false when health reaches 0
};

class Arena{
private:
    Config m_config;
    std::vector<std::vector<cell>> m_board;  // 2D board of cells
    std::vector<RobotEntry> m_robots;     // All robots currently loaded into the game
    int m_round = 1;
public:
    Arena();
    ~Arena();
    bool load_config(const std::string& filename);   // Reads config file and fills m_config
    void load_robots(const std::string& robotDir);  // Compile and load all Robot_*.cpp files from robots folder
    void initialize_board();    // Creates an empty board using config height/width
    void place_obstacles(); // Randomly place M, P, and F obstacles
    void place_robots();     // Place robots randomly on valid empty cells
    void print_board() const;   // Print current game board
    void run_game();    // Main game loop
private:
    // Utility checks
    bool inBounds(int row, int col) const;
    bool isCellFreeForRobot(int row, int col) const;

    // Convert a cell to display text
    std::string getCellDisplay(int row, int col) const;

    // Radar logic
    std::vector<RadarObj> performRadarScan(int robotIndex, int direction) const;

    // Turn actions
    bool handleShot(int robotIndex, int targetRow, int targetCol);
    void handleMove(int robotIndex, int direction, int distance);

    // Damage and game status
    void applyDamage(int robotIndex, int rawDamage);
    int countLivingRobots() const;
    int getWinningRobotIndex() const;

    // Placement helpers
    void placeSingleObstacle(Terrian type);

    // Board helpers
    char getObjectTypeAt(int row, int col) const;
};