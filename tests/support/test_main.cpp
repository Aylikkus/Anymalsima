#include "test.hpp"

int main(int argc, char** argv) {
    if (argc != 2 || !test::cases().contains(argv[1])) {
        std::cerr << "Supply a registered test name. Available cases:\n";
        for (const auto& [name, fn] : test::cases()) std::cerr << name << '\n';
        return 2;
    }
    try {
        test::cases().at(argv[1])();
        std::cout << "PASS " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << argv[1] << ": " << error.what() << '\n';
        return 1;
    }
}
