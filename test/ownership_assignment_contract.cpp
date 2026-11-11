#include <cstdio>
#include <memory>
#include <utility>

#include "memory/lite_ptr.h"
#include "utility/callable_wrapper.h"
#include "task/task_wrapper.h"
#include "flow/flow_async_aggregator.h"

using namespace flux_foundry;

namespace {
struct node;
using node_ptr = lite_ptr<node, default_deleter<node>, alignof(void*)>;
struct node {
    node_ptr* next;
    int* destroyed;
    int value;
    node(node_ptr* n, int* d, int v) noexcept : next(n), destroyed(d), value(v) {}
    ~node();
};
node::~node() { ++*destroyed; delete next; }

bool nested_pointer_assignment(bool move) {
    int parent_destroyed = 0, child_destroyed = 0;
    node_ptr owner(in_place, new node_ptr(in_place, nullptr, &child_destroyed, 42),
                   &parent_destroyed, 0);
    if (move) owner = std::move(*owner->next);
    else owner = *owner->next;
    const bool ok = parent_destroyed == 1 && child_destroyed == 0 && owner &&
                    owner->value == 42 && owner.use_count() == 1;
    owner.release();
    return ok && child_destroyed == 1;
}

bool pointer_assignment_edges() {
    int destroyed = 0;
    node_ptr a(in_place, nullptr, &destroyed, 42);
    auto b = a;
    a = b;
    node_ptr& alias = a;
    a = alias;
    a = std::move(alias);
    if (destroyed || a.use_count() != 2 || b.use_count() != 2) return false;
    a = std::move(b);
    if (b || a.use_count() != 1 || destroyed) return false;
    node_ptr empty;
    a = empty;
    if (a || destroyed != 1) return false;
    a = std::move(empty);
    return !a && !empty && destroyed == 1;
}

struct intrusive_node;
using intrusive_ptr = detail::intrusive_awaitable_ptr<intrusive_node>;
struct intrusive_node {
    std::atomic<unsigned> refs{0};
    intrusive_ptr* next;
    int* destroyed;
    int value;
    intrusive_node(intrusive_ptr* n, int* d, int v) noexcept : next(n), destroyed(d), value(v) {}
    ~intrusive_node() { ++*destroyed; delete next; }
    void retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

bool nested_intrusive_assignment(bool move) {
    int parent_destroyed = 0, child_destroyed = 0;
    intrusive_ptr owner(new intrusive_node(
        new intrusive_ptr(new intrusive_node(nullptr, &child_destroyed, 42)), &parent_destroyed, 0));
    if (move) owner = std::move(*owner->next);
    else owner = *owner->next;
    const bool ok = parent_destroyed == 1 && child_destroyed == 0 && owner &&
                    owner->value == 42 && owner->refs.load() == 1;
    owner.reset();
    return ok && child_destroyed == 1;
}

bool intrusive_assignment_edges() {
    int destroyed = 0;
    intrusive_ptr a(new intrusive_node(nullptr, &destroyed, 42)), b(a);
    a = b;
    intrusive_ptr& alias = a;
    a = alias;
    a = std::move(alias);
    if (destroyed || a->refs.load() != 2 || b.get() != a.get()) return false;
    a = std::move(b);
    if (b || a->refs.load() != 1 || destroyed) return false;
    intrusive_ptr empty;
    a = empty;
    a = std::move(empty);
    return !a && !empty && destroyed == 1;
}

template <std::size_t Padding>
struct owned_callable {
    char padding[Padding]{};
    std::unique_ptr<int> value;
    int* destroyed;
    owned_callable(int n, int* d) : value(new int(n)), destroyed(d) {}
    owned_callable(owned_callable&&) noexcept = default;
    owned_callable(const owned_callable&) = delete;
    ~owned_callable() { if (value) ++*destroyed; }
    int operator()() const noexcept { return *value; }
};

template <typename Wrapper, std::size_t Padding>
bool callable_assignment_edges() {
    int destroyed = 0;
    Wrapper source{owned_callable<Padding>(42, &destroyed)}, destination;
    destination = std::move(source);
    if (source || !destination || destination() != 42 || destroyed) return false;
    Wrapper& alias = destination;
    destination = std::move(alias);
    if (destination() != 42 || destroyed) return false;
    Wrapper replacement{owned_callable<Padding>(7, &destroyed)};
    destination = std::move(replacement);
    if (replacement || destination() != 7 || destroyed != 1) return false;
    destination = std::move(source);
    if (source || destination || destroyed != 2) return false;
    source = std::move(destination);
    return !source && !destination && destroyed == 2;
}

struct nested_callable {
    callable_wrapper<int()> child;
    callable_wrapper<int()>** exposed;
    explicit nested_callable(callable_wrapper<int()>** out) noexcept
        : child([]() noexcept { return 42; }), exposed(out) { *exposed = &child; }
    nested_callable(nested_callable&& other) noexcept
        : child(std::move(other.child)), exposed(other.exposed) { *exposed = &child; }
    nested_callable(const nested_callable&) = delete;
    int operator()() noexcept { return child(); }
};

bool nested_callable_assignment() {
    callable_wrapper<int()>* child = nullptr;
    callable_wrapper<int()> owner{nested_callable(&child)};
    owner = std::move(*child);
    return owner && owner() == 42;
}

bool task_assignment() {
    int value = 0;
    task_wrapper_sbo source([&value]() noexcept { ++value; }), destination;
    destination = std::move(source);
    if (source || !destination) return false;
    destination();
    task_wrapper_sbo replacement([&value]() noexcept { value += 10; });
    destination = std::move(replacement);
    if (replacement) return false;
    destination();
    destination = std::move(source);
    return !destination && value == 11;
}
} // namespace

int main() {
    bool ok = true;
    auto check = [&ok](const char* name, bool result) {
        std::printf("[%s] %s\n", result ? "OK" : "FAIL", name);
        ok = result && ok;
    };
    check("copy a pointer owned by the replaced pointee", nested_pointer_assignment(false));
    check("move a pointer owned by the replaced pointee", nested_pointer_assignment(true));
    check("pointer self/shared/empty assignments", pointer_assignment_edges());
    check("copy an intrusive pointer owned by the replaced pointee", nested_intrusive_assignment(false));
    check("move an intrusive pointer owned by the replaced pointee", nested_intrusive_assignment(true));
    check("intrusive pointer self/shared/empty assignments", intrusive_assignment_edges());
    check("SBO callable ownership", callable_assignment_edges<callable_wrapper<int()>, 1>());
    check("heap callable ownership", callable_assignment_edges<callable_wrapper<int()>, 128>());
    check("const SBO callable ownership", callable_assignment_edges<callable_wrapper<int() const>, 1>());
    check("const heap callable ownership", callable_assignment_edges<callable_wrapper<int() const>, 128>());
    check("move a callable owned by the replaced callable", nested_callable_assignment());
    check("task assignment ownership", task_assignment());
    return ok ? 0 : 1;
}
