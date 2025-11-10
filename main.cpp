#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <random>
#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <array>

class SafeStruct {
private:
    std::array<int, 2> data{0, 0};
    mutable std::array<std::mutex, 2> mutexes;

public:
    void set(int field, int value) {
        if (field < 0 || field >= (int)data.size()) return;
        std::unique_lock<std::mutex> lock(mutexes[field]);
        data[field] = value;
    }

    int get(int field) const {
        if (field < 0 || field >= (int)data.size()) return 0;
        std::unique_lock<std::mutex> lock(mutexes[field]);
        return data[field];
    }

    explicit operator std::string() const {
        std::unique_lock<std::mutex> lock0(mutexes[0], std::defer_lock);
        std::unique_lock<std::mutex> lock1(mutexes[1], std::defer_lock);
        std::lock(lock0, lock1);

        return std::to_string(data[0]) + " " + std::to_string(data[1]);
    }
};

enum class Op { READ, WRITE, STR };

struct Action {
    Op op;
    int field = -1;
    int value = 0;
};

void generate_file(const std::string& filename, size_t num_ops, const std::vector<double>& probs) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::discrete_distribution<int> dist(probs.begin(), probs.end());

    std::ofstream out(filename);
    if (!out) {
        std::cerr << "Cannot open " << filename << " for writing\n";
        return;
    }
    for (size_t i = 0; i < num_ops; ++i) {
        int op_type = dist(gen);
        switch (op_type) {
            case 0: out << "read 0\n"; break;
            case 1: out << "write 0 1\n"; break;
            case 2: out << "read 1\n"; break;
            case 3: out << "write 1 1\n"; break;
            default: out << "string\n"; break;
        }
    }
}

std::vector<Action> parse_file(const std::string& filename) {
    std::vector<Action> actions;
    std::ifstream in(filename);
    if (!in) {
        std::cerr << "Cannot open " << filename << " for reading\n";
        return actions;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "read") {
            int f;
            iss >> f;
            actions.push_back({Op::READ, f});
        } else if (cmd == "write") {
            int f, v;
            iss >> f >> v;
            actions.push_back({Op::WRITE, f, v});
        } else if (cmd == "string") {
            actions.push_back({Op::STR});
        }
    }
    return actions;
}

void execute(const std::vector<Action>& actions, SafeStruct& s) {
    for (const auto& a : actions) {
        if (a.op == Op::READ) {
            volatile int val = s.get(a.field);
            (void)val;
        } else if (a.op == Op::WRITE) {
            s.set(a.field, a.value);
        } else {
            std::string tmp = static_cast<std::string>(s);
            (void)tmp;
        }
    }
}

void run_once(SafeStruct& s, const std::vector<std::vector<Action>>& action_lists) {
    std::vector<std::thread> threads;
    threads.reserve(action_lists.size());
    for (const auto& actions : action_lists) {
        threads.emplace_back(execute, std::cref(actions), std::ref(s));
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

double measure_time(const std::vector<std::vector<Action>>& action_lists) {
    SafeStruct s;
    auto start = std::chrono::steady_clock::now();
    run_once(s, action_lists);
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

void generate_all_files(
    size_t num_ops,
    int max_threads,
    const std::array<std::vector<double>, 3>& all_probs,
    const std::array<std::string, 3>& tags)
{
    for (int t = 1; t <= max_threads; ++t) {
        for (int variant = 0; variant < 3; ++variant) {
            size_t base = num_ops / t;
            size_t rem = num_ops % t;
            for (int i = 0; i < t; ++i) {
                size_t nops = base + (i < (int)rem ? 1 : 0);
                std::string filename =
                    "input_" + tags[variant] + "_" + std::to_string(t) + "_" + std::to_string(i) + ".txt";
                generate_file(filename, nops, all_probs[variant]);
            }
        }
    }
}

void measure_all(
    double results[3][3],
    int max_threads,
    const std::array<std::vector<double>, 3>& all_probs,
    const std::array<std::string, 3>& tags,
    int repeats = 5)
{
    for (int variant = 0; variant < 3; ++variant) {
        for (int t = 1; t <= max_threads; ++t) {
            double total_time = 0.0;

            for (int r = 0; r < repeats; ++r) {
                std::vector<std::vector<Action>> action_lists;
                for (int i = 0; i < t; ++i) {
                    std::string filename =
                        "input_" + tags[variant] + "_" + std::to_string(t) + "_" + std::to_string(i) + ".txt";
                    action_lists.push_back(parse_file(filename));
                }

                total_time += measure_time(action_lists);
            }

            results[variant][t - 1] = total_time / repeats;
        }
    }
}

void print_results(
    const double results[3][3],
    const std::array<std::string, 3>& tags)
{
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Variant\\Threads |  1 thread  |  2 threads |  3 threads\n";
    std::cout << "---------------------------------------------------\n";
    for (int v = 0; v < 3; ++v) {
        std::cout << std::left << std::setw(13) << tags[v] << " | ";
        for (int t = 0; t < 3; ++t) {
            std::cout << std::right << std::setw(10) << results[v][t] << " | ";
        }
        std::cout << "\n";
    }
}

int main() {
    const size_t num_ops = 400000;
    const int max_threads = 3;
    int runs = 5;

    std::vector<double> probsA = {0.10, 0.05, 0.50, 0.10, 0.25};
    std::vector<double> probsB = {0.20, 0.20, 0.20, 0.20, 0.20};
    std::vector<double> probsC = {0.20, 0.10, 0.05, 0.20, 0.45};

    std::array<std::vector<double>, 3> all_probs = {probsA, probsB, probsC};
    std::array<std::string, 3> tags = {"A_variant", "B_variant", "C_variant"};

    generate_all_files(num_ops, max_threads, all_probs, tags);

    double results[3][3] = {}; // [variant][threads-1]
    measure_all(results, max_threads, all_probs, tags, runs );

    print_results(results, tags);

    return 0;
}
