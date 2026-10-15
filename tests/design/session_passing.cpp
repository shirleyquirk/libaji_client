// Evidence for docs/cpp-wrapper/DESIGN.md §5: how a non-copyable, non-movable
// Session can be passed to a user callback.
//
// Built by tests/design/run.sh three times:
//   (default)  must compile and run; prints when the destructor (the handback)
//              runs relative to the callback returning
//   -DBAD1     must NOT compile: a by-value parameter, given a named local
//   -DBAD2     must NOT compile: a Session&& parameter, given a named local
#include <cstdio>
#include <utility>

struct Session {
    int id;
    explicit Session(int i) : id(i) {}
    Session(const Session&) = delete;
    Session(Session&&) = delete;
    ~Session() { std::puts("~Session (handback)"); }
};

// Recommended: named local, passed as an lvalue.
template<class F> auto with_named(F&& f)
{
    Session s{1};
    auto r = f(s);
    std::puts("after f, before return");
    return r;
}

// Binding to && needs no move constructor: std::move is only a cast.
template<class F> auto with_rref(F&& f) { Session s{1}; return f(std::move(s)); }

// The trap: a prvalue lets a by-value parameter compile (guaranteed elision),
// and the destructor then runs at an implementation-defined point.
template<class F> auto with_prvalue(F&& f) { return f(Session{2}); }

int main()
{
    with_named([](Session& s) { return s.id; });
    with_named([](auto& s) { return s.id; });
    with_rref([](Session&& s) { return s.id; });
    with_prvalue([](Session s) { return s.id; });
#ifdef BAD1
    with_named([](Session s) { return s.id; });    // error: copy constructor deleted
#endif
#ifdef BAD2
    with_named([](Session&& s) { return s.id; });  // error: lvalue doesn't bind to Session&&
#endif
}
