#include <atomic>
#include <cstddef>
enum control_flag { idle=0, running=1 };
int main(){
  std::atomic<size_t> s{0};
  auto curr = control_flag::idle;
  return s.compare_exchange_strong(curr, control_flag::running) ? 0 : 1;
}
