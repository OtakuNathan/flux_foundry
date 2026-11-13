#include <exception>
#include <stdexcept>
int main() {
    auto error = std::make_exception_ptr(std::logic_error("probe"));
    return error ? 0 : 1;
}
