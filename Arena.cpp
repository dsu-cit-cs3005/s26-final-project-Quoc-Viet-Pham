#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "Arena.h"

namespace {
constexpr std::size_t kMaxRobotSummaryChars = 50;

std::string trim_copy(const std::string& text)
{
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

int direction_from_delta(int delta_row, int delta_col)
{
    const int row_step = (delta_row > 0) - (delta_row < 0);
    const int col_step = (delta_col > 0) - (delta_col < 0);

    for (int direction = 1; direction <= 8; ++direction) {
        if (directions[direction].first == row_step &&
            directions[direction].second == col_step) {
            return direction;
        }
    }

    return 0;
}

std::pair<int, int> get_side_offsets(int direction)
{
    const int dr = directions[direction].first;
    const int dc = directions[direction].second;

    if (dr != 0 && dc == 0) {
        return {0, 1};
    }

    if (dr == 0 && dc != 0) {
        return {1, 0};
    }

    return {-dc, dr};
}

std::vector<std::pair<int, int>> build_bresenham_ray(
    int start_row,
    int start_col,
    int target_row,
    int target_col,
    int row_max,
    int col_max,
    int max_cells = -1)
{
    std::vector<std::pair<int, int>> cells;

    if (start_row == target_row && start_col == target_col) {
        return cells;
    }

    int x = start_col;
    int y = start_row;
    const int x1 = target_col;
    const int y1 = target_row;
    const int dx = std::abs(x1 - x);
    const int sx = (x < x1) ? 1 : -1;
    const int dy = -std::abs(y1 - y);
    const int sy = (y < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        const int e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y += sy;
        }

        if (y < 0 || y >= row_max || x < 0 || x >= col_max) {
            break;
        }

        cells.emplace_back(y, x);

        if (max_cells > 0 && static_cast<int>(cells.size()) >= max_cells) {
            break;
        }
    }

    return cells;
}

int roll_damage(WeaponType weapon)
{
    switch (weapon) {
        case railgun:
            return 10 + (std::rand() % 11);
        case hammer:
            return 50 + (std::rand() % 11);
        case grenade:
            return 10 + (std::rand() % 31);
        case flamethrower:
            return 30 + (std::rand() % 21);
    }

    return 0;
}
} // namespace

Arena::Arena() = default;

Arena::~Arena()
{
    for (RobotEntry& entry : m_robots) {
        delete entry.robot;
        entry.robot = nullptr;

        if (entry.handle != nullptr) {
            dlclose(entry.handle);
            entry.handle = nullptr;
        }
    }
}

// Reads config file and fills m_config
bool Arena::load_config(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Failed to open config file: " << filename << "\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(line.substr(0, colon));
        const std::string value = trim_copy(line.substr(colon + 1));

        if (key == "Arena_Size") {
            std::istringstream iss(value);
            int height = 0;
            int width = 0;
            if (iss >> height >> width && height > 0 && width > 0) {
                m_config.height = height;
                m_config.width = width;
            }
        }
        else if (key == "Max_Rounds") {
            m_config.max_rounds = std::max(1, std::stoi(value));
        }
        else if (key == "Sleep_interval") {
            m_config.sleep_interval = std::max(0.0, std::stod(value));
        }
        else if (key == "Game_State_Live") {
            m_config.game_state_live = (value == "true" || value == "TRUE");
        }
        else if (key == "Flamethrowers") {
            m_config.flamethrowers = std::max(0, std::stoi(value));
        }
        else if (key == "Pits") {
            m_config.pits = std::max(0, std::stoi(value));
        }
        else if (key == "Mounds") {
            m_config.mounts = std::max(0, std::stoi(value));
        }
    }

    return true;
}

// Compile and load all Robot_*.cpp files from robots folder
void Arena::load_robots(const std::string& robotDir)
{
    namespace fs = std::filesystem;
    using RobotSummaryFn = const char* (*)();

    if (!fs::exists(robotDir) || !fs::is_directory(robotDir)) {
        std::cerr << "Robot directory not found: " << robotDir << "\n";
        return;
    }

    std::vector<fs::path> robot_sources;
    for (const auto& entry : fs::directory_iterator(robotDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (filename.rfind("Robot_", 0) == 0 && entry.path().extension() == ".cpp") {
            robot_sources.push_back(entry.path());
        }
    }

    std::sort(robot_sources.begin(), robot_sources.end());

    const std::string symbols = "@#$%&*!+^~?";
    int symbol_index = static_cast<int>(m_robots.size());

    for (const fs::path& path : robot_sources) {
        const std::string source_file = path.generic_string();
        const fs::path lib_path = path.parent_path() / ("lib" + path.stem().string() + ".so");
        const std::string lib_file = lib_path.generic_string();

        const std::string compile_cmd =
            "g++ -shared -fPIC -o \"" + lib_file + "\" \"" + source_file +
            "\" RobotBase.o -I. -std=c++20 -Wall -Wextra -pedantic";

        if (std::system(compile_cmd.c_str()) != 0) {
            std::cerr << "Failed to compile " << source_file << "\n";
            continue;
        }

        void* handle = dlopen(lib_file.c_str(), RTLD_LAZY);
        if (handle == nullptr) {
            std::cerr << "Failed to load " << lib_file << ": " << dlerror() << "\n";
            continue;
        }

        RobotFactory create_robot =
            reinterpret_cast<RobotFactory>(dlsym(handle, "create_robot"));
        if (create_robot == nullptr) {
            std::cerr << "Failed to find create_robot in " << lib_file << "\n";
            dlclose(handle);
            continue;
        }

        RobotSummaryFn summary_fn =
            reinterpret_cast<RobotSummaryFn>(dlsym(handle, "robot_summary"));
        if (summary_fn == nullptr) {
            std::cerr << "Failed to find robot_summary in " << lib_file << "\n";
            dlclose(handle);
            continue;
        }

        const char* summary = summary_fn();
        if (summary == nullptr || std::strlen(summary) == 0 ||
            std::strlen(summary) > kMaxRobotSummaryChars) {
            std::cerr << "Invalid robot_summary in " << lib_file << "\n";
            dlclose(handle);
            continue;
        }

        RobotBase* robot = create_robot();
        if (robot == nullptr) {
            std::cerr << "Failed to create robot from " << lib_file << "\n";
            dlclose(handle);
            continue;
        }

        if (robot->m_name.empty() || robot->m_name == "Blank_Robot") {
            robot->m_name = path.stem().string();
        }

        RobotEntry new_entry;
        new_entry.robot = robot;
        new_entry.handle = handle;
        new_entry.symbol =
            (symbol_index < static_cast<int>(symbols.size())) ? symbols[symbol_index++] : '?';
        new_entry.alive = true;

        m_robots.push_back(new_entry);
        std::cout << "Loaded " << robot->m_name << ": " << summary << "\n";
    }
}

// Creates an empty board using config height/width
void Arena::initialize_board()
{
    m_board.assign(m_config.height, std::vector<cell>(m_config.width));
}

// Randomly place M, P, and F obstacles
void Arena::place_obstacles()
{
    for (int i = 0; i < m_config.flamethrowers; ++i) {
        placeSingleObstacle(Terrian::Flamethrower);
    }

    for (int i = 0; i < m_config.pits; ++i) {
        placeSingleObstacle(Terrian::Pit);
    }

    for (int i = 0; i < m_config.mounts; ++i) {
        placeSingleObstacle(Terrian::Mound);
    }
}

// Place robots randomly on valid empty cells
void Arena::place_robots()
{
    for (int i = 0; i < static_cast<int>(m_robots.size()); ++i) {
        while (true) {
            const int row = std::rand() % m_config.height;
            const int col = std::rand() % m_config.width;

            if (isCellFreeForRobot(row, col)) {
                m_board[row][col].robot_index = i;
                m_robots[i].robot->move_to(row, col);
                m_robots[i].robot->set_boundaries(m_config.height, m_config.width);
                m_robots[i].robot->m_character = m_robots[i].symbol;
                break;
            }
        }
    }
}

void Arena::print_board() const
{
    std::cout << "\n===== Round " << m_round << " =====\n\n";
    std::cout << "   ";
    for (int col = 0; col < m_config.width; ++col) {
        std::cout << std::setw(3) << col;
    }
    std::cout << "\n";

    for (int row = 0; row < m_config.height; ++row) {
        std::cout << std::setw(2) << row << " ";
        for (int col = 0; col < m_config.width; ++col) {
            std::cout << std::setw(3) << getCellDisplay(row, col);
        }
        std::cout << "\n";
    }

    std::cout << "\n";
    for (const RobotEntry& entry : m_robots) {
        if (entry.robot == nullptr) {
            continue;
        }

        std::cout << entry.robot->print_stats();
        if (!entry.alive) {
            std::cout << " [OUT]";
        }
        std::cout << "\n";
    }
}

// Print current game board
void Arena::run_game()
{
    if (m_robots.empty()) {
        std::cout << "No robots were loaded.\n";
        return;
    }

    while (m_round <= m_config.max_rounds) {
        for (int i = 0; i < static_cast<int>(m_robots.size()); ++i) {
            if (countLivingRobots() <= 1) {
                if (m_config.game_state_live) {
                    print_board();
                }

                const int winner = getWinningRobotIndex();
                if (winner != -1) {
                    std::cout << "Winner: " << m_robots[winner].robot->m_name << "\n";
                }
                else {
                    std::cout << "No winner.\n";
                }
                return;
            }

            if (m_config.game_state_live) {
                print_board();
            }

            if (!m_robots[i].alive) {
                ++m_round;
                continue;
            }

            int radar_direction = 0;
            m_robots[i].robot->get_radar_direction(radar_direction);
            const std::vector<RadarObj> radar_results = performRadarScan(i, radar_direction);
            m_robots[i].robot->process_radar_results(radar_results);

            int shot_row = 0;
            int shot_col = 0;

            if (m_robots[i].robot->get_shot_location(shot_row, shot_col)) {
                handleShot(i, shot_row, shot_col);
            }
            else {
                int move_direction = 0;
                int move_distance = 0;
                m_robots[i].robot->get_move_direction(move_direction, move_distance);
                handleMove(i, move_direction, move_distance);
            }

            if (m_config.game_state_live && m_config.sleep_interval > 0.0) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(m_config.sleep_interval));
            }

            ++m_round;
        }
    }

    if (m_config.game_state_live) {
        print_board();
    }

    std::cout << "Maximum rounds reached.\n";
    const int winner = getWinningRobotIndex();
    if (winner != -1) {
        std::cout << "Winner: " << m_robots[winner].robot->m_name << "\n";
    }
}

// Utility checks
bool Arena::inBounds(int row, int col) const
{
    return row >= 0 && row < m_config.height &&
           col >= 0 && col < m_config.width;
}

bool Arena::isCellFreeForRobot(int row, int col) const
{
    if (!inBounds(row, col)) {
        return false;
    }

    const cell& current = m_board[row][col];
    return current.terrian == Terrian::Empty && current.robot_index == -1;
}

// Convert a cell to display text
std::string Arena::getCellDisplay(int row, int col) const
{
    const cell& current = m_board[row][col];

    if (current.robot_index != -1) {
        const RobotEntry& robot_entry = m_robots[current.robot_index];
        return robot_entry.alive ? std::string("R") + robot_entry.symbol
                                 : std::string("X") + robot_entry.symbol;
    }

    switch (current.terrian) {
        case Terrian::Empty:
            return ".";
        case Terrian::Mound:
            return "M";
        case Terrian::Pit:
            return "P";
        case Terrian::Flamethrower:
            return "F";
    }

    return "?";
}

// Radar logic
std::vector<RadarObj> Arena::performRadarScan(int robotIndex, int direction) const
{
    std::vector<RadarObj> results;

    if (robotIndex < 0 || robotIndex >= static_cast<int>(m_robots.size())) {
        return results;
    }

    int robot_row = 0;
    int robot_col = 0;
    m_robots[robotIndex].robot->get_current_location(robot_row, robot_col);

    if (direction == 0) {
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) {
                    continue;
                }

                const int scan_row = robot_row + dr;
                const int scan_col = robot_col + dc;
                if (!inBounds(scan_row, scan_col)) {
                    continue;
                }

                const char object_type = getObjectTypeAt(scan_row, scan_col);
                if (object_type != '.') {
                    results.emplace_back(object_type, scan_row, scan_col);
                }
            }
        }

        return results;
    }

    if (direction < 1 || direction > 8) {
        return results;
    }

    const int dr = directions[direction].first;
    const int dc = directions[direction].second;
    const auto [side_row, side_col] = get_side_offsets(direction);

    for (int distance = 1;; ++distance) {
        bool found_in_bounds = false;

        for (int offset = -1; offset <= 1; ++offset) {
            const int scan_row = robot_row + dr * distance + side_row * offset;
            const int scan_col = robot_col + dc * distance + side_col * offset;

            if (!inBounds(scan_row, scan_col)) {
                continue;
            }

            found_in_bounds = true;

            const char object_type = getObjectTypeAt(scan_row, scan_col);
            if (object_type != '.') {
                results.emplace_back(object_type, scan_row, scan_col);
            }
        }

        if (!found_in_bounds) {
            break;
        }
    }

    return results;
}

// Turn actions
bool Arena::handleShot(int robotIndex, int targetRow, int targetCol)
{
    if (robotIndex < 0 || robotIndex >= static_cast<int>(m_robots.size()) ||
        !m_robots[robotIndex].alive) {
        return false;
    }

    RobotBase* robot = m_robots[robotIndex].robot;
    const WeaponType weapon = robot->get_weapon();

    if (m_config.game_state_live) {
        std::cout << robot->m_name << " fires at (" << targetRow << ", " << targetCol << ")\n";
    }

    int shooter_row = 0;
    int shooter_col = 0;
    robot->get_current_location(shooter_row, shooter_col);

    if (weapon == railgun) {
        if (!inBounds(targetRow, targetCol)) {
            return false;
        }

        const int raw_damage = roll_damage(railgun);
        const auto ray = build_bresenham_ray(
            shooter_row,
            shooter_col,
            targetRow,
            targetCol,
            m_config.height,
            m_config.width);

        for (const auto& [row, col] : ray) {
            const int target_index = m_board[row][col].robot_index;
            if (target_index != -1 && target_index != robotIndex && m_robots[target_index].alive) {
                applyDamage(target_index, raw_damage);
            }
        }

        return true;
    }

    if (weapon == grenade) {
        if (!inBounds(targetRow, targetCol) || robot->get_grenades() <= 0) {
            return false;
        }

        robot->decrement_grenades();
        const int raw_damage = roll_damage(grenade);

        for (int row = targetRow - 1; row <= targetRow + 1; ++row) {
            for (int col = targetCol - 1; col <= targetCol + 1; ++col) {
                if (!inBounds(row, col)) {
                    continue;
                }

                const int target_index = m_board[row][col].robot_index;
                if (target_index != -1 && m_robots[target_index].alive) {
                    applyDamage(target_index, raw_damage);
                }
            }
        }

        return true;
    }

    if (weapon == flamethrower) {
        const int direction = direction_from_delta(targetRow - shooter_row, targetCol - shooter_col);
        if (direction == 0) {
            return false;
        }

        const int raw_damage = roll_damage(flamethrower);
        const int dr = directions[direction].first;
        const int dc = directions[direction].second;
        const auto [side_row, side_col] = get_side_offsets(direction);

        for (int distance = 1; distance <= 4; ++distance) {
            for (int offset = -1; offset <= 1; ++offset) {
                const int row = shooter_row + dr * distance + side_row * offset;
                const int col = shooter_col + dc * distance + side_col * offset;

                if (!inBounds(row, col)) {
                    continue;
                }

                const int target_index = m_board[row][col].robot_index;
                if (target_index != -1 && target_index != robotIndex && m_robots[target_index].alive) {
                    applyDamage(target_index, raw_damage);
                }
            }
        }

        return true;
    }

    if (weapon == hammer) {
        const int row_gap = std::abs(targetRow - shooter_row);
        const int col_gap = std::abs(targetCol - shooter_col);
        if ((row_gap == 0 && col_gap == 0) || row_gap > 1 || col_gap > 1) {
            return false;
        }

        const int raw_damage = roll_damage(hammer);

        for (int row = shooter_row - 1; row <= shooter_row + 1; ++row) {
            for (int col = shooter_col - 1; col <= shooter_col + 1; ++col) {
                if (!inBounds(row, col) || (row == shooter_row && col == shooter_col)) {
                    continue;
                }

                const int target_index = m_board[row][col].robot_index;
                if (target_index != -1 && target_index != robotIndex && m_robots[target_index].alive) {
                    applyDamage(target_index, raw_damage);
                }
            }
        }

        return true;
    }

    return false;
}

void Arena::handleMove(int robotIndex, int direction, int distance)
{
    if (!m_robots[robotIndex].alive || direction < 1 || direction > 8) {
        return;
    }

    RobotBase* robot = m_robots[robotIndex].robot;
    const int max_move = robot->get_move_speed();
    distance = std::min(distance, max_move);

    int current_row = 0;
    int current_col = 0;
    robot->get_current_location(current_row, current_col);

    const int start_row = current_row;
    const int start_col = current_col;
    const int dr = directions[direction].first;
    const int dc = directions[direction].second;

    for (int step = 0; step < distance; ++step) {
        const int next_row = current_row + dr;
        const int next_col = current_col + dc;

        if (!inBounds(next_row, next_col)) {
            break;
        }

        cell& next_cell = m_board[next_row][next_col];
        if (next_cell.robot_index != -1 || next_cell.terrian == Terrian::Mound) {
            break;
        }

        m_board[current_row][current_col].robot_index = -1;
        next_cell.robot_index = robotIndex;
        robot->move_to(next_row, next_col);

        current_row = next_row;
        current_col = next_col;

        if (next_cell.terrian == Terrian::Pit) {
            robot->disable_movement();
            break;
        }

        if (next_cell.terrian == Terrian::Flamethrower) {
            applyDamage(robotIndex, roll_damage(flamethrower));

            if (!m_robots[robotIndex].alive) {
                next_cell.terrian = Terrian::Empty;
                break;
            }
        }
    }

    if (m_config.game_state_live && (start_row != current_row || start_col != current_col)) {
        std::cout << robot->m_name << " moves to (" << current_row << ", " << current_col << ")\n";
    }
}

// Damage and game status
void Arena::applyDamage(int robotIndex, int rawDamage)
{
    RobotBase* robot = m_robots[robotIndex].robot;
    const int armor_before = robot->get_armor();
    const double reduction = armor_before * 0.1;
    int final_damage = static_cast<int>(rawDamage * (1.0 - reduction));

    if (final_damage < 0) {
        final_damage = 0;
    }

    robot->take_damage(final_damage);
    robot->reduce_armor(1);

    if (m_config.game_state_live) {
        std::cout << "  " << robot->m_name << " takes " << final_damage
                  << " damage (armor " << armor_before << " -> " << robot->get_armor()
                  << ", health " << robot->get_health() << ")\n";
    }

    if (robot->get_health() <= 0) {
        m_robots[robotIndex].alive = false;
        if (m_config.game_state_live) {
            std::cout << "  " << robot->m_name << " is out.\n";
        }
    }
}

int Arena::countLivingRobots() const
{
    int count = 0;
    for (const RobotEntry& robot : m_robots) {
        if (robot.alive) {
            ++count;
        }
    }
    return count;
}

int Arena::getWinningRobotIndex() const
{
    int winner = -1;

    for (int i = 0; i < static_cast<int>(m_robots.size()); ++i) {
        if (m_robots[i].alive) {
            if (winner != -1) {
                return -1;
            }
            winner = i;
        }
    }

    return winner;
}

// Placement helpers
void Arena::placeSingleObstacle(Terrian type)
{
    while (true) {
        const int row = std::rand() % m_config.height;
        const int col = std::rand() % m_config.width;

        if (m_board[row][col].terrian == Terrian::Empty &&
            m_board[row][col].robot_index == -1) {
            m_board[row][col].terrian = type;
            return;
        }
    }
}

// Board helpers
char Arena::getObjectTypeAt(int row, int col) const
{
    const cell& current = m_board[row][col];

    if (current.robot_index != -1) {
        return m_robots[current.robot_index].alive ? 'R' : 'X';
    }

    switch (current.terrian) {
        case Terrian::Empty:
            return '.';
        case Terrian::Mound:
            return 'M';
        case Terrian::Pit:
            return 'P';
        case Terrian::Flamethrower:
            return 'F';
    }

    return '?';
}
