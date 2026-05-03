#include "RobotBase.h"

#include <array>
#include <deque>
#include <limits>
#include <set>
#include <utility>
#include <vector>

class Robot_Vex : public RobotBase
{
private:
    std::set<std::pair<int, int>> m_hazards;
    std::deque<std::pair<int, int>> m_recent_positions;

    bool m_has_target = false;
    int m_target_row = -1;
    int m_target_col = -1;

    int m_last_seen_row = -1;
    int m_last_seen_col = -1;
    int m_last_seen_turn = -1000;

    int m_turn = 0;
    int m_sweep_index = 0;

    static int manhattan(int row_a, int col_a, int row_b, int col_b)
    {
        return std::abs(row_a - row_b) + std::abs(col_a - col_b);
    }

    static int chebyshev(int row_a, int col_a, int row_b, int col_b)
    {
        return std::max(std::abs(row_a - row_b), std::abs(col_a - col_b));
    }

    int direction_toward(int from_row, int from_col, int to_row, int to_col) const
    {
        const int row_step = (to_row > from_row) - (to_row < from_row);
        const int col_step = (to_col > from_col) - (to_col < from_col);

        for (int direction = 1; direction <= 8; ++direction) {
            if (directions[direction].first == row_step &&
                directions[direction].second == col_step) {
                return direction;
            }
        }

        return 0;
    }

    bool in_bounds(int row, int col) const
    {
        return row >= 0 && row < m_board_row_max &&
               col >= 0 && col < m_board_col_max;
    }

    void remember_hazard(const RadarObj& obj)
    {
        if (obj.m_type == 'M' || obj.m_type == 'P' ||
            obj.m_type == 'F' || obj.m_type == 'X') {
            m_hazards.insert({obj.m_row, obj.m_col});
        }
    }

    bool is_hazard(int row, int col) const
    {
        return m_hazards.find({row, col}) != m_hazards.end();
    }

    bool recently_visited(int row, int col) const
    {
        for (const auto& [visited_row, visited_col] : m_recent_positions) {
            if (visited_row == row && visited_col == col) {
                return true;
            }
        }
        return false;
    }

    void remember_position(int row, int col)
    {
        if (!m_recent_positions.empty() && m_recent_positions.back() == std::pair(row, col)) {
            return;
        }

        m_recent_positions.emplace_back(row, col);
        if (m_recent_positions.size() > 8) {
            m_recent_positions.pop_front();
        }
    }

    int score_candidate(int row, int col) const
    {
        const int center_row = m_board_row_max / 2;
        const int center_col = m_board_col_max / 2;

        int score = 0;
        score -= manhattan(row, col, center_row, center_col) * 3;

        if (row == 0 || col == 0 ||
            row == m_board_row_max - 1 || col == m_board_col_max - 1) {
            score -= 4;
        }

        if (recently_visited(row, col)) {
            score -= 7;
        }

        int nearest_hazard = std::numeric_limits<int>::max();
        for (const auto& [hazard_row, hazard_col] : m_hazards) {
            nearest_hazard = std::min(
                nearest_hazard,
                chebyshev(row, col, hazard_row, hazard_col));
        }

        if (nearest_hazard != std::numeric_limits<int>::max()) {
            score += std::min(nearest_hazard, 6);
        }

        if (m_turn - m_last_seen_turn <= 3) {
            const int spacing = chebyshev(row, col, m_last_seen_row, m_last_seen_col);
            score -= std::abs(spacing - 4) * 2;
        }

        return score;
    }

public:
    Robot_Vex() : RobotBase(2, 5, grenade)
    {
        m_name = "Vex";
    }

    void get_radar_direction(int& radar_direction) override
    {
        int current_row = 0;
        int current_col = 0;
        get_current_location(current_row, current_col);
        remember_position(current_row, current_col);

        if (m_turn - m_last_seen_turn <= 2) {
            const int chase_direction =
                direction_toward(current_row, current_col, m_last_seen_row, m_last_seen_col);
            if (chase_direction != 0) {
                radar_direction = chase_direction;
                return;
            }
        }

        const int center_row = m_board_row_max / 2;
        const int center_col = m_board_col_max / 2;
        if (manhattan(current_row, current_col, center_row, center_col) >
            (m_board_row_max + m_board_col_max) / 6) {
            const int inward_direction =
                direction_toward(current_row, current_col, center_row, center_col);
            if (inward_direction != 0) {
                radar_direction = inward_direction;
                return;
            }
        }

        static constexpr std::array<int, 9> sweep_order = {0, 3, 5, 7, 1, 2, 4, 6, 8};
        radar_direction = sweep_order[m_sweep_index];
        m_sweep_index = (m_sweep_index + 1) % static_cast<int>(sweep_order.size());
    }

    void process_radar_results(const std::vector<RadarObj>& radar_results) override
    {
        ++m_turn;
        m_has_target = false;

        int current_row = 0;
        int current_col = 0;
        get_current_location(current_row, current_col);

        int best_distance = std::numeric_limits<int>::max();

        for (const RadarObj& obj : radar_results) {
            remember_hazard(obj);

            if (obj.m_type == 'R') {
                const int distance = chebyshev(current_row, current_col, obj.m_row, obj.m_col);
                if (distance < best_distance) {
                    best_distance = distance;
                    m_target_row = obj.m_row;
                    m_target_col = obj.m_col;
                    m_has_target = true;
                }
            }
        }

        if (m_has_target) {
            m_last_seen_row = m_target_row;
            m_last_seen_col = m_target_col;
            m_last_seen_turn = m_turn;
        }
    }

    bool get_shot_location(int& shot_row, int& shot_col) override
    {
        if (!m_has_target || get_grenades() <= 0) {
            return false;
        }

        int current_row = 0;
        int current_col = 0;
        get_current_location(current_row, current_col);

        if (std::abs(current_row - m_target_row) <= 1 &&
            std::abs(current_col - m_target_col) <= 1) {
            return false;
        }

        shot_row = m_target_row;
        shot_col = m_target_col;
        return true;
    }

    void get_move_direction(int& move_direction, int& move_distance) override
    {
        if (get_move_speed() <= 0) {
            move_direction = 0;
            move_distance = 0;
            return;
        }

        int start_row = 0;
        int start_col = 0;
        get_current_location(start_row, start_col);

        int best_direction = 0;
        int best_distance = 0;
        int best_score = std::numeric_limits<int>::min();
        std::pair<int, int> best_destination = {start_row, start_col};

        for (int direction = 1; direction <= 8; ++direction) {
            int row = start_row;
            int col = start_col;
            const int dr = directions[direction].first;
            const int dc = directions[direction].second;

            for (int distance = 1; distance <= get_move_speed(); ++distance) {
                const int next_row = row + dr;
                const int next_col = col + dc;

                if (!in_bounds(next_row, next_col) || is_hazard(next_row, next_col)) {
                    break;
                }

                row = next_row;
                col = next_col;

                const int score = score_candidate(row, col) + distance;
                if (score > best_score) {
                    best_score = score;
                    best_direction = direction;
                    best_distance = distance;
                    best_destination = {row, col};
                }
            }
        }

        move_direction = best_direction;
        move_distance = best_distance;

        if (best_direction != 0) {
            remember_position(best_destination.first, best_destination.second);
        }
    }
};

extern "C" RobotBase* create_robot()
{
    return new Robot_Vex();
}

extern "C" const char* robot_summary()
{
    return "Tracks targets, holds center, grenades on sight.";
}
