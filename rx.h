#pragma once

#include <sstream>

namespace rx {
    
// based on Walter Brown's void_t proposal
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n3911.pdf
namespace detail {
    template<class... TN> struct void_type {typedef void type;};
}
template<class... TN>
using void_t = typename detail::void_type<TN...>::type;

namespace shapes {

template<class Payload>
struct state;

template<class Payload, class Clock>
struct context;

struct subscription
{
    bool is_stopped();
    void stop();

    void insert(const subscription& s);
    void erase(const subscription& s);

    void insert(function<void()> stopper);

    template<class Payload, class... ArgN>
    state<Payload> make_state(ArgN... argn);
    template<class Payload>
    state<Payload> copy_state(const state<Payload>&);
};

template<class Payload>
struct state {
    subscription lifetime;
    Payload& get();
};

struct starter {
    template<class Payload, class Clock>
    subscription start(context<Payload, Clock>);
};

struct observer {
    template<class T>
    void next(T);
    template<class E>
    void error(E);
    void complete();
};

struct subscriber {
    template<class Payload, class Clock>
    observer create(context<Payload, Clock>);
};

struct observable {
    starter bind(subscriber);
};

struct lifter {
    subscriber lift(subscriber);
};

struct adaptor {
    observable adapt(observable);
};

struct terminator {
    starter terminate(observable);
};

template<class Clock>
struct strand {
    subscription lifetime;

    typename Clock::time_point now();
    void defer_at(typename Clock::time_point, observer);
};

template<class Clock>
void defer(strand<Clock>, observer);
template<class Clock>
void defer_at(strand<Clock>, typename Clock::time_point, observer);
template<class Clock>
void defer_after(strand<Clock>, typename Clock::duration, observer);
template<class Clock>
void defer_periodic(strand<Clock>, typename Clock::time_point, typename Clock::duration, observer);

template<class Payload, class Clock>
struct context {
    subscription lifetime;
    
    typename Clock::time_point now();
    void defer_at(typename Clock::time_point, observer);

    Payload& get();
};

template<class Payload, class Clock>
void defer(context<Payload, Clock>, observer);
template<class Payload, class Clock>
void defer_at(context<Payload, Clock>, typename Clock::time_point, observer);
template<class Payload, class Clock>
void defer_after(context<Payload, Clock>, typename Clock::duration, observer);
template<class Payload, class Clock>
void defer_periodic(context<Payload, Clock>, typename Clock::time_point, typename Clock::duration, observer);

}

using std::decay_t;

template<class Payload = void>
struct state;

///
/// \brief A subscription represents the scope of an async operation. 
/// Holds a set of nested lifetimes. 
/// Can be used to make state that is scoped to the subscription. 
/// Can call arbitratry functions at the end of the lifetime.
///
struct subscription
{
private:
    struct shared
    {
        ~shared(){
            auto expired = std::move(destructors);
            for (auto& d : expired) {
                d();
            }
        }
        shared() : stopped(false) {info("new lifetime");}
        bool stopped;
        set<subscription> others;
        deque<function<void()>> stoppers;
        deque<function<void()>> destructors;
    };
public:
    subscription() : store(make_shared<shared>()) {}
    explicit subscription(shared_ptr<shared> o) : store(o) {}
    /// \brief used to exit loops or otherwise stop work scoped to this subscription.
    /// \returns bool - if true do not access any state objects.
    bool is_stopped() const {
        return store->stopped;
    }
    /// \brief 
    void insert(const subscription& s) const {
        if (s == *this) {std::abort();}
        // nest
        store->others.insert(s);
        // unnest when child is stopped
        weak_ptr<shared> p = store;
        weak_ptr<shared> c = s.store;
        s.insert([p, c](){
            auto storep = p.lock();
            auto storec = c.lock();
            if (storep && storec) {
                auto that = subscription(storep);
                auto s = subscription(storec);
                that.erase(s);
            }
        });
        if (store->stopped) stop();
    }
    /// \brief 
    void erase(const subscription& s) const {
        if (s == *this) {std::abort();}
        store->others.erase(s);
    }
    /// \brief 
    void insert(function<void()> stopper) const {
        store->stoppers.emplace_front(stopper);
        if (store->stopped) stop();
    }
    /// \brief 
    template<class Payload, class... ArgN>
    state<Payload> make_state(ArgN... argn) const;
    /// \brief 
    state<> make_state() const;
    /// \brief 
    state<> copy_state(const state<>&) const;
    /// \brief 
    template<class Payload>
    state<Payload> copy_state(const state<Payload>&) const;
    /// \brief 
    void stop() const {
        store->stopped = true;
        {
            auto others = std::move(store->others);
            for (auto& o : others) {
                o.stop();
            }
        }
        {
            auto stoppers = std::move(store->stoppers);
            for (auto& s : stoppers) {
                s();
            }
        }
    }
private:
    shared_ptr<shared> store;
    friend bool operator==(const subscription&, const subscription&);
    friend bool operator<(const subscription&, const subscription&);
};
bool operator==(const subscription& lhs, const subscription& rhs) {
    return lhs.store == rhs.store;
}
bool operator!=(const subscription& lhs, const subscription& rhs) {
    return !(lhs == rhs);
}
bool operator<(const subscription& lhs, const subscription& rhs) {
    return lhs.store < rhs.store;
}

namespace detail {

template<class T>
struct subscription_check : public false_type {};

template<>
struct subscription_check<subscription> : public true_type {};

template<class T>
using for_subscription = enable_if_t<subscription_check<std::decay_t<T>>::value>;

template<class T>
using not_subscription = enable_if_t<!subscription_check<std::decay_t<T>>::value>;

}

template<>
struct state<void>
{
    subscription lifetime;
    explicit state(subscription lifetime) 
        : lifetime(lifetime) {
    }
    state(const state&) = default;
    template<class Payload>
    state(const state<Payload>& o)
        : lifetime(o.lifetime) {
    }
};

template<class Payload>
struct state
{
    subscription lifetime;
    state(subscription l, Payload* p) : lifetime(l), p(p) {}
    Payload& get() {
        return *p;
    }
    Payload& get() const {
        return *p;
    }
    explicit operator state<>(){
        return {lifetime};
    }
private:
    mutable Payload* p;
};

template<class Payload, class... ArgN>
state<Payload> subscription::make_state(ArgN... argn) const {
    auto p = make_unique<Payload>(argn...);
    auto result = state<Payload>{*this, p.get()};
    store->destructors.emplace_front(
        [d=p.release()]() mutable {
            auto p = d; 
            d = nullptr; 
            delete p;
        });
    return result;
}
state<> subscription::make_state() const {
    auto result = state<>{*this};
    return result;
}

state<> subscription::copy_state(const state<>&) const{
    return make_state();
}

template<class Payload>
state<Payload> subscription::copy_state(const state<Payload>& o) const{
    return make_state<Payload>(o.get());
}

template<class Payload, class... ArgN>
state<Payload> make_state(subscription lifetime, ArgN... argn) {
    return lifetime.template make_state<Payload>(forward<ArgN>(argn)...);
}
inline state<> make_state(subscription lifetime) {
    return lifetime.make_state();
}

state<> copy_state(subscription lifetime, const state<>&) {
    return lifetime.make_state();
}

template<class Payload>
state<Payload> copy_state(subscription lifetime, const state<Payload>& o) {
    return lifetime.template make_state<Payload>(o.get());
}

namespace detail {

template<class T>
struct state_check : public false_type {};

template<class Payload>
struct state_check<state<Payload>> : public true_type {};

template<class T>
using for_state = enable_if_t<state_check<std::decay_t<T>>::value>;

template<class T>
using not_state = enable_if_t<!state_check<std::decay_t<T>>::value>;

}

template<class Next, class Error, class Complete, class Delegatee = void>
struct observer;

namespace detail {
    
template<class T>
struct observer_check : public false_type {};

template<class Next, class Error, class Complete, class Delegatee>
struct observer_check<observer<Next, Error, Complete, Delegatee>> : public true_type {};

template<class T>
using for_observer = enable_if_t<observer_check<decay_t<T>>::value>;

template<class T>
using not_observer = enable_if_t<!observer_check<decay_t<T>>::value>;

auto report = [](auto&& e, auto&& f, auto&&... args){
    try{f(args...);} catch(...) {e(current_exception());}
};

auto enforce = [](const subscription& lifetime, auto&& f) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) f(args...);
    };
};

auto end = [](const subscription& lifetime, auto&& f, auto&&... cap) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) { 
            f(cap..., args...); 
            lifetime.stop();
        }
    };
};

struct noop
{
    template<class V, class CheckV = not_observer<V>>
    void operator()(V&&) const {
    }
    template<class Delegatee, class V>
    void operator()(const Delegatee& d, V&& v) const {
        d.next(std::forward<V>(v));
    }
    inline void operator()() const {
    }
    template<class Delegatee, class Check = for_observer<Delegatee>>
    void operator()(const Delegatee& d) const {
        d.complete();
    }
};
struct ignore
{
    template<class E>
    void operator()(E&&) const {
    }
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee& d, E&& e) const {
        d.error(forward<E>(e));
    }
};
struct fail
{
    template<class E>
    void operator()(E&&) const {
        info("abort! ");
        std::abort();
    }
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee&, E&&) const {
        info("abort! ");
        std::abort();
    }
};

}

namespace detail{
    template<class V, class E>
    struct abstract_observer
    {
        virtual ~abstract_observer(){}
        virtual void next(const V&) const = 0;
        virtual void error(const E&) const = 0;
        virtual void complete() const = 0;
    };

    template<class V, class E, class Next, class Error, class Complete, class Delegatee>
    struct basic_observer : public abstract_observer<V, E> {
        using value_t = decay_t<V>;
        using errorvalue_t = decay_t<E>;
        basic_observer(const observer<Next, Error, Complete, Delegatee>& o)
            : d(o){
        }
        observer<Next, Error, Complete, Delegatee> d;
        virtual void next(const value_t& v) const {
            d.next(v);
        }
        virtual void error(const errorvalue_t& err) const {
            d.error(err);
        }
        virtual void complete() const {
            d.complete();
        }
    };
}
template<class V, class E>
struct observer<V, E, void, void> {
    using value_t = decay_t<V>;
    using errorvalue_t = decay_t<E>;
    observer(const observer& o) = default;
    template<class Next, class Error, class Complete, class Delegatee>
    observer(const observer<Next, Error, Complete, Delegatee>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_observer<V, E, Next, Error, Complete, Delegatee>>(o)) {
    }
    subscription lifetime;
    shared_ptr<detail::abstract_observer<value_t, errorvalue_t>> d;
    void next(const value_t& v) const {
        d->next(v);
    }
    void error(const errorvalue_t& err) const {
        d->error(err);
    }
    void complete() const {
        d->complete();
    }
    template<class... TN>
    observer as_interface() const {
        return {*this};
    }
};
template<class V, class E>
using observer_interface = observer<V, E, void, void>;


template<class Next, class Error, class Complete>
struct observer<Next, Error, Complete, void> {
    subscription lifetime;
    mutable Next n;
    mutable Error e;
    mutable Complete c;
    template<class V>
    void next(V&& v) const {
        using namespace detail;
        report(end(lifetime, e), enforce(lifetime, n), std::forward<V>(v));
    }
    template<class E>
    void error(E&& err) const {
        using namespace detail;
        report(fail{}, end(lifetime, e), std::forward<E>(err));
    }
    void complete() const {
        using namespace detail;
        report(fail{}, end(lifetime, c));
    }
    template<class V, class E = exception_ptr>
    observer_interface<V, E> as_interface() const {
        using observer_t = detail::basic_observer<V, E, Next, Error, Complete, void>;
        return {lifetime, make_shared<observer_t>(*this)};
    }
};
template<class Delegatee, class Next, class Error, class Complete>
struct observer {
    Delegatee d;
    subscription lifetime;
    mutable Next n;
    mutable Error e;
    mutable Complete c;
    template<class V>
    void next(V&& v) const {
        using namespace detail;
        report(end(lifetime, e, d), enforce(lifetime, n), d, std::forward<V>(v));
    }
    template<class E>
    void error(E&& err) const {
        using namespace detail;
        report(fail{}, end(lifetime, e), d, std::forward<E>(err));
    }
    void complete() const {
        using namespace detail;
        report(fail{}, end(lifetime, c), d);
    }
    template<class V, class E = exception_ptr>
    observer_interface<V, E> as_interface() const {
        using observer_t = detail::basic_observer<V, E, Delegatee, Next, Error, Complete>;
        return {lifetime, make_shared<observer_t>(*this)};
    }
};

template<class Next = detail::noop, class Error = detail::fail, class Complete = detail::noop, 
    class CheckN = detail::not_observer<Next>>
auto make_observer(subscription lifetime, Next&& n = Next{}, Error&& e = Error{}, Complete&& c = Complete{}) {
    return observer<decay_t<Next>, decay_t<Error>, decay_t<Complete>, void>{
        lifetime,
        forward<Next>(n), 
        forward<Error>(e), 
        forward<Complete>(c)
    };
}

template<class Delegatee, class Next = detail::noop, class Error = detail::fail, class Complete = detail::noop, 
    class CheckD = detail::for_observer<Delegatee>>
auto make_observer(Delegatee&& d, subscription lifetime, Next&& n = Next{}, Error&& e = Error{}, Complete&& c = Complete{}) {
    return observer<decay_t<Delegatee>, decay_t<Next>, decay_t<Error>, decay_t<Complete>>{
        forward<Delegatee>(d), 
        lifetime,
        forward<Next>(n), 
        forward<Error>(e), 
        forward<Complete>(c)
    };
}

template<class Execute, class Now, class Clock>
struct strand;

namespace detail {
    template<class C>
    using re_defer_at_t = function<void(typename C::time_point)>;

    template<class C, class E>
    struct abstract_strand
    {
        virtual ~abstract_strand(){}
        virtual typename C::time_point now() const = 0;
        virtual void defer_at(typename C::time_point, observer_interface<re_defer_at_t<C>, E>) const = 0;
    };

    template<class C, class E, class Execute, class Now>
    struct basic_strand : public abstract_strand<C, E> {
        using clock_t = decay_t<C>;
        using errorvalue_t = decay_t<E>;
        basic_strand(const strand<Execute, Now, C>& o)
            : d(o){
        }
        strand<Execute, Now, C> d;
        virtual typename C::time_point now() const {
            return d.now();
        }
        virtual void defer_at(typename C::time_point at, observer_interface<re_defer_at_t<C>, E> out) const {
            d.defer_at(at, out);
        }
    };

    template<class Clock>
    struct immediate {
        subscription lifetime;
        template<class Next, class Error, class Complete, class Delegatee>
        void operator()(typename Clock::time_point at, observer<Next, Error, Complete, Delegatee> out) const {
            auto next = at;
            bool stop = false;
            info("immediate::defer_at");
            while (!stop && !lifetime.is_stopped()) {
                info("immediate::defer_at sleep_until");
                this_thread::sleep_until(next);
                stop = true;
                info("immediate::defer_at next");
                out.next([&](typename Clock::time_point at){
                    info("immediate::defer_at self");
                    stop = false;
                    next = at;
                });
            }
            info("immediate::defer_at complete");
            out.complete();
        }
    };

    template<class Clock>
    struct now {
        typename Clock::time_point operator()() const {
            return Clock::now();
        }
    };
}

template<class C, class E>
struct strand<C, E, void_t<typename C::time_point>> {
    using clock_t = decay_t<C>;
    using errorvalue_t = decay_t<E>;
    strand(const strand& o) = default;
    template<class Execute, class Now>
    strand(const strand<Execute, Now, C>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_strand<C, E, Execute, Now>>(o)) {
    }
    subscription lifetime;
    shared_ptr<detail::abstract_strand<clock_t, errorvalue_t>> d;
    typename C::time_point now() const {
        return d->now();
    }
    void defer_at(typename C::time_point at, observer_interface<detail::re_defer_at_t<C>, E> out) const {
        d->defer_at(at, out);
    }
    template<class... TN>
    strand as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using strand_interface = strand<C, E, void>;

template<class Execute, class Now, class Clock>
struct strand {
    subscription lifetime;
    Execute e;
    Now n;
    typename Clock::time_point now() const {
        return n();
    }
    template<class Next, class Error, class Complete, class Delegatee>
    void defer_at(typename Clock::time_point at, observer<Next, Error, Complete, Delegatee> out) const {
        e(at, out);
    }
    template<class E = exception_ptr>
    strand_interface<Clock, E> as_interface() const {
        using strand_t = detail::basic_strand<Clock, E, Execute, Now>;
        return {lifetime, make_shared<strand_t>(*this)};
    }
};

template<class Clock = steady_clock, class Execute = detail::immediate<Clock>, class Now = detail::now<Clock>>
auto make_strand(subscription lifetime, Execute&& e = Execute{}, Now&& n = Now{}) {
    return strand<decay_t<Execute>, decay_t<Now>, Clock>{
        lifetime,
        forward<Execute>(e), 
        forward<Now>(n)
    };
}

namespace detail {

template<class T>
struct strand_check : public false_type {};

template<class Execute, class Now, class Clock>
struct strand_check<strand<Execute, Now, Clock>> : public true_type {};

template<class T>
using for_strand = enable_if_t<strand_check<std::decay_t<T>>::value>;

template<class T>
using not_strand = enable_if_t<!strand_check<std::decay_t<T>>::value>;

}

template<class Execute, class Now, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer(strand<Execute, Now, Clock> s, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(s.now(), out);
}
template<class Execute, class Now, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_at(strand<Execute, Now, Clock> s, typename Clock::time_point at, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(at, out);
}
template<class Execute, class Now, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_after(strand<Execute, Now, Clock> s, typename Clock::duration delay, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(s.now() + delay, out);
}
template<class Execute, class Now, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_periodic(strand<Execute, Now, Clock> s, typename Clock::time_point initial, typename Clock::duration period, observer<Next, Error, Complete, Delagatee> out) {
    long count = 0;
    auto target = initial;
    s.defer_at(initial, make_observer(
        out, 
        out.lifetime, 
        [count, target, period](observer<Next, Error, Complete, Delagatee>& out, auto& self) mutable {
            if (!out.lifetime.is_stopped()) {
                out.next(count++);
                target += period;
                self(target);
            }
        },
        [](const observer<Next, Error, Complete, Delagatee>& out, exception_ptr ep){
            return out.error(ep);
        },
        [](const observer<Next, Error, Complete, Delagatee>& out){
            // don't shut down out
        }));
}

template<class Payload = void, class MakeStrand = void, class Clock = steady_clock>
struct context;

namespace detail {
    template<class C, class E>
    struct abstract_context : public abstract_strand<C, E>
    {
        virtual ~abstract_context(){}
    };

    template<class C, class E, class MakeStrand>
    struct basic_context : public abstract_context<C, E> {
        using clock_t = decay_t<C>;
        using errorvalue_t = decay_t<E>;
        basic_context(context<void, MakeStrand, C> o)
            : d(o){
        }
        context<void, MakeStrand, C> d;
        virtual typename C::time_point now() const {
            return d.now();
        }
        virtual void defer_at(typename C::time_point at, observer_interface<re_defer_at_t<C>, E> out) const {
            d.defer_at(at, out);
        }
    };
    
    template<class C, class E>
    using make_strand_t = function<strand_interface<C, E>(subscription)>;

    template<class Clock>
    struct make_immediate {
        auto operator()(subscription lifetime) const {
            return make_strand<Clock>(lifetime, detail::immediate<Clock>{}, detail::now<Clock>{});
        }
    };

}
template<class C, class E>
struct context<C, E, void_t<typename C::time_point>> {
    using clock_t = decay_t<C>;
    using errorvalue_t = decay_t<E>;
    context(const context& o) = default;
    template<class Payload, class MakeStrand, class R = decltype(declval<MakeStrand>()(declval<subscription>()))>
    context(const context<Payload, MakeStrand, C>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_context<C, E, MakeStrand>>(o))
        , m([m = o.m](subscription lifetime){
            return m(lifetime);
        }) {
    }
    subscription lifetime;
    shared_ptr<detail::abstract_context<clock_t, errorvalue_t>> d;
    detail::make_strand_t<C, E> m;
    typename C::time_point now() const {
        return d->now();
    }
    void defer_at(typename C::time_point at, observer_interface<detail::re_defer_at_t<C>, E> out) const {
        d->defer_at(at, out);
    }
    template<class... TN>
    context as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using context_interface = context<C, E, void>;

template<class Clock>
struct context<void, void, Clock> {
    using MakeStrand = detail::make_immediate<Clock>;
    subscription lifetime;
    MakeStrand m;
private:
    using Strand = decay_t<decltype(declval<MakeStrand>()(declval<subscription>()))>;
    struct State {
        explicit State(Strand& s) : s(s) {}
        Strand s;
    };
    state<State> s;    
public:

    explicit context(subscription lifetime) 
        : lifetime(lifetime)
        , m()
        , s(make_state<State>(lifetime, m(lifetime))) {
    }
    context(subscription lifetime, MakeStrand m, Strand s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
    }
    typename Clock::time_point now() const {
        return s.get().s.now();
    }
    template<class Next, class Error, class Complete, class Delegatee>
    void defer_at(typename Clock::time_point at, observer<Next, Error, Complete, Delegatee> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<Clock, E> as_interface() const {
        using context_t = detail::basic_context<Clock, E, MakeStrand>;
        return {lifetime, make_shared<context_t>(*this)};
    }
};

template<class MakeStrand, class Clock>
struct context<void, MakeStrand, Clock> {
    subscription lifetime;
    MakeStrand m;

private:
    using Strand = decay_t<decltype(declval<MakeStrand>()(declval<subscription>()))>;
    struct State {
        State(Strand s) : s(s) {}
        Strand s;
    };
    state<State> s;  

public:
    context(subscription lifetime, MakeStrand m) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, m(lifetime))) {
    }
    context(subscription lifetime, MakeStrand m, Strand s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
    }
    typename Clock::time_point now() const {
        return s.get().s.now();
    }
    template<class Next, class Error, class Complete, class Delegatee>
    void defer_at(typename Clock::time_point at, observer<Next, Error, Complete, Delegatee> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<Clock, E> as_interface() const {
        using context_t = detail::basic_context<Clock, E, MakeStrand>;
        return {lifetime, make_shared<context_t>(*this)};
    }  
};

template<class Payload, class MakeStrand, class Clock>
struct context {
    subscription lifetime;
    MakeStrand m;
    context(subscription lifetime, Payload p, MakeStrand m) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, m(lifetime), p)) {
    }
    typename Clock::time_point now() const {
        return s.get().s.now();
    }
    template<class Next, class Error, class Complete, class Delegatee>
    void defer_at(typename Clock::time_point at, observer<Next, Error, Complete, Delegatee> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<Clock, E> as_interface() const {
        using context_t = detail::basic_context<Clock, E, MakeStrand>;
        return {lifetime, make_shared<context_t>(*this)};
    }
    Payload& get(){
        return s.get().p;
    }
    const Payload& get() const {
        return s.get().p;
    }
    operator context<void, MakeStrand, Clock> () const {
        return context<void, MakeStrand, Clock>(lifetime, m, s.get().s);
    }
private:
    using Strand = decay_t<decltype(declval<MakeStrand>()(declval<subscription>()))>;
    struct State {
        explicit State(Strand& s, Payload& p) : s(s), p(p) {}
        Strand s;
        Payload p;
    };
    state<State> s;    
};

inline auto make_context(subscription lifetime) {
    return context<void, void, steady_clock>{
        lifetime
    };
}

template<class Clock = steady_clock, class MakeStrand>
auto make_context(subscription lifetime, MakeStrand&& m) {
    return context<void, decay_t<MakeStrand>, Clock>{
        lifetime,
        forward<MakeStrand>(m) 
    };
}

template<class Payload, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    return context<Payload, detail::make_immediate<steady_clock>, steady_clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<steady_clock>{}
    };
}

template<class Payload, class Clock, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    return context<Payload, detail::make_immediate<Clock>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<Clock>{}
    };
}

template<class Payload, class Clock, class MakeStrand, class... AN>
auto make_context(subscription lifetime, MakeStrand&& m, AN&&... an) {
    return context<Payload, decay_t<MakeStrand>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        forward<MakeStrand>(m)
    };
}

inline auto copy_context(subscription lifetime, const context<>&) {
    return make_context(lifetime);
}

template<class Payload, class MakeStrand, class Clock>
auto copy_context(subscription lifetime, const context<Payload, MakeStrand, Clock>& o) {
    return make_context<Payload, Clock>(lifetime, o.m, o.get());
}

template<class MakeStrand, class Clock>
auto copy_context(subscription lifetime, const context<void, MakeStrand, Clock>& o) {
    return make_context<Clock>(lifetime, o.m);
}

template<class C, class E>
auto copy_context(subscription lifetime, const context_interface<C, E>& o) {
    return context_interface<C, E>{
        context<void, detail::make_strand_t<C, E>, C> {
            lifetime,
            o.m
        }
    };
}

namespace detail {

template<class T>
struct context_check : public false_type {};

template<class Payload, class MakeStrand, class Clock>
struct context_check<context<Payload, MakeStrand, Clock>> : public true_type {};

template<class T>
using for_context = enable_if_t<context_check<std::decay_t<T>>::value>;

template<class T>
using not_context = enable_if_t<!context_check<std::decay_t<T>>::value>;

}

template<class Payload, class MakeStrand, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer(context<Payload, MakeStrand, Clock> s, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(s.now(), out);
}
template<class Payload, class MakeStrand, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_at(context<Payload, MakeStrand, Clock> s, typename Clock::time_point at, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(at, out);
}
template<class Payload, class MakeStrand, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_after(context<Payload, MakeStrand, Clock> s, typename Clock::duration delay, observer<Next, Error, Complete, Delagatee> out) {
    s.defer_at(s.now() + delay, out);
}
template<class Payload, class MakeStrand, class Clock, class Next, class Error, class Complete, class Delagatee>
void defer_periodic(context<Payload, MakeStrand, Clock> s, typename Clock::time_point initial, typename Clock::duration period, observer<Next, Error, Complete, Delagatee> out) {
    long count = 0;
    auto target = initial;
    s.defer_at(initial, make_observer(
        out, 
        out.lifetime, 
        [count, target, period](const observer<Next, Error, Complete, Delagatee>& out, auto& self) mutable {
            out.next(count++);
            target += period;
            self(target);
        },
        [](const observer<Next, Error, Complete, Delagatee>& out, exception_ptr ep){
            return out.error(ep);
        },
        [](const observer<Next, Error, Complete, Delagatee>& out){
            // don't shut down out
        }));
}

template<class Start>
struct starter;

namespace detail {
    template<class C, class E>
    using start_t = function<subscription(context_interface<C, E>)>;
}
template<class C, class E>
struct starter<detail::start_t<C, E>> {
    detail::start_t<C, E> s;
    starter(const starter&) = default;
    template<class Start>
    starter(const starter<Start>& s)
        : s(s.s) {
    }
    subscription start(context_interface<C, E> ctx) const {
        return s(move(ctx));
    }
    template<class... TN>
    starter as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using starter_interface = starter<detail::start_t<C, E>>;

template<class Start>
struct starter {
    Start s;
    template<class Payload, class MakeStrand, class Clock>
    subscription start(context<Payload, MakeStrand, Clock> ctx) const {
        return s(ctx);
    }
    template<class C = steady_clock, class E = exception_ptr>
    starter_interface<C, E> as_interface() const {
        return {*this};
    }
};
template<class Start>
starter<Start> make_starter(Start&& s) {
    return {forward<Start>(s)};
}

namespace detail {

template<class T>
struct starter_check : public false_type {};

template<class Start>
struct starter_check<starter<Start>> : public true_type {};

template<class T>
using for_starter = enable_if_t<starter_check<decay_t<T>>::value>;

template<class T>
using not_starter = enable_if_t<!starter_check<decay_t<T>>::value>;

}

template<class Create>
struct subscriber;

namespace detail {
    template<class V, class C, class E>
    using create_t = function<observer_interface<V, E>(context_interface<C, E>)>;
}
template<class V, class C, class E>
struct subscriber<detail::create_t<V, C, E>> {
    detail::create_t<V, C, E> c;
    subscriber(const subscriber&) = default;
    template<class Create>
    subscriber(const subscriber<Create>& o)
        : c(o.c) {
    }
    observer_interface<V, E> create(context_interface<C, E> ctx) const;
    template<class... TN>
    subscriber as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using subscriber_interface = subscriber<detail::create_t<V, C, E>>;

template<class Create>
struct subscriber {
    Create c;
    /// \brief returns observer
    template<class Payload, class MakeStrand, class Clock>
    auto create(context<Payload, MakeStrand, Clock> ctx) const {
        return c(ctx);
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    subscriber_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Create>
subscriber<Create> make_subscriber(Create&& c) {
    return {forward<Create>(c)};
}

auto make_subscriber() {
    return make_subscriber([](auto ctx){return make_observer(ctx.lifetime);});
}

namespace detail {

template<class T>
struct subscriber_check : public false_type {};

template<class Create>
struct subscriber_check<subscriber<Create>> : public true_type {};

template<class T>
using for_subscriber = enable_if_t<subscriber_check<decay_t<T>>::value>;

template<class T>
using not_subscriber = enable_if_t<!subscriber_check<decay_t<T>>::value>;

}

template<class Bind>
struct observable;

namespace detail {
    template<class V, class C, class E>
    using bind_t = function<starter_interface<C, E>(const subscriber_interface<V, C, E>&)>;
}
template<class V, class C, class E>
struct observable<detail::bind_t<V, C, E>> {
    detail::bind_t<V, C, E> b;
    observable(const observable&) = default;
    template<class Bind>
    observable(const observable<Bind>& o) 
        : b(o.b) {
    } 
    starter_interface<C, E> bind(const subscriber_interface<V, C, E>& s) const {
        return b(s);
    }
    template<class... TN>
    observable as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using observable_interface = observable<detail::bind_t<V, C, E>>;

template<class Bind>
struct observable {
    Bind b;
    /// \brief 
    /// \returns starter
    template<class Subscriber>
    auto bind(Subscriber&& s) const {
        return b(s);
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    observable_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Bind>
observable<Bind> make_observable(Bind&& b) {
    return {forward<Bind>(b)};
}

namespace detail {

template<class T>
struct observable_check : public false_type {};

template<class Bind>
struct observable_check<observable<Bind>> : public true_type {};

template<class T>
using for_observable = enable_if_t<observable_check<decay_t<T>>::value>;

template<class T>
using not_observable = enable_if_t<!observable_check<decay_t<T>>::value>;

}

template<class Lift>
struct lifter;

namespace detail {
    template<class VL, class CL, class EL, class VR, class CR, class ER>
    using lift_t = function<subscriber_interface<VL, CL, EL>(const subscriber_interface<VR, CR, ER>&)>;
}
template<class VL, class CL, class EL, class VR, class CR, class ER>
struct lifter<detail::lift_t<VL, CL, EL, VR, CR, ER>> {
    detail::lift_t<VL, CL, EL, VR, CR, ER> l;
    lifter(const lifter&) = default;
    template<class Lift>
    lifter(const lifter<Lift>& l) 
        : l(l.l){
    }
    subscriber_interface<VL, CL, EL> lift(const subscriber_interface<VR, CR, ER>& s) const {
        return l(s);
    }
    template<class... TN>
    lifter as_interface() const {
        return {*this};
    }
};
template<class VL, class CL, class EL, class VR, class CR, class ER>
using lifter_interface = lifter<detail::lift_t<VL, CL, EL, VR, CR, ER>>;

template<class Lift>
struct lifter {
    Lift l;
    /// \brief returns subscriber    
    template<class Subscriber>
    auto lift(Subscriber&& s) const {
        return l(forward<Subscriber>(s));
    }
    template<class VL, class CL = steady_clock, class EL = exception_ptr, class VR = VL, class CR = CL, class ER = EL>
    lifter_interface<VL, CL, EL, VR, CR, ER> as_interface() const {
        return {*this};
    }
};

template<class Lift>
lifter<Lift> make_lifter(Lift&& l) {
    return {forward<Lift>(l)};
}

namespace detail {

template<class T>
struct lifter_check : public false_type {};

template<class Lift>
struct lifter_check<lifter<Lift>> : public true_type {};

template<class T>
using for_lifter = enable_if_t<lifter_check<decay_t<T>>::value>;

template<class T>
using not_lifter = enable_if_t<!lifter_check<decay_t<T>>::value>;

}

template<class Adapt>
struct adaptor;

namespace detail {
    template<class VL, class CL, class EL, class VR, class CR, class ER>
    using adapt_t = function<observable_interface<VR, CR, ER>(const observable_interface<VL, CL, EL>&)>;
}
template<class VL, class CL, class EL, class VR, class CR, class ER>
struct adaptor<detail::adapt_t<VL, CL, EL, VR, CR, ER>> {
    detail::adapt_t<VL, CL, EL, VR, CR, ER> a;
    adaptor(const adaptor&) = default;
    template<class Adapt>
    adaptor(const adaptor<Adapt>& a)
        : a(a.a) {
    }
    observable_interface<VR, CR, ER> adapt(const observable_interface<VL, CL, EL>& ovr) const {
        return a(ovr);
    }
    template<class... TN>
    adaptor as_interface() const {
        return {*this};
    }
};
template<class VL, class CL, class EL, class VR, class CR, class ER>
using adaptor_interface = adaptor<detail::adapt_t<VL, CL, EL, VR, CR, ER>>;

template<class Adapt>
struct adaptor {
    Adapt a;
    /// \brief returns observable
    template<class Observable>
    auto adapt(Observable&& s) const {
        return a(forward<Observable>(s));
    }
    template<class VL, class CL = steady_clock, class EL = exception_ptr, class VR = VL, class CR = CL, class ER = EL>
    adaptor_interface<VL, CL, EL, VR, CR, ER> as_interface() const {
        return {*this};
    }
};

template<class Adapt>
adaptor<Adapt> make_adaptor(Adapt&& l) {
    return {forward<Adapt>(l)};
}

namespace detail {

template<class T>
struct adaptor_check : public false_type {};

template<class Adapt>
struct adaptor_check<adaptor<Adapt>> : public true_type {};

template<class T>
using for_adaptor = enable_if_t<adaptor_check<decay_t<T>>::value>;

template<class T>
using not_adaptor = enable_if_t<!adaptor_check<decay_t<T>>::value>;

}

template<class Terminate>
struct terminator;

namespace detail {
    template<class V, class C, class E>
    using terminate_t = function<starter_interface<C, E>(const observable_interface<V, C, E>&)>;
}
template<class V, class C, class E>
struct terminator<detail::terminate_t<V, C, E>> {
    detail::terminate_t<V, C, E> t;
    terminator(const terminator&) = default;
    template<class Terminate>
    terminator(const terminator<Terminate>& t) 
        : t(t.t) {
    }
    starter_interface<C, E> terminate(const observable_interface<V, C, E>& ovr) const {
        return t(ovr);
    }
    template<class... TN>
    terminator as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using terminator_interface = terminator<detail::terminate_t<V, C, E>>;

template<class Terminate>
struct terminator {
    Terminate t;
    /// \brief returns starter
    template<class Observable>
    auto terminate(Observable&& s) const {
        return t(forward<Observable>(s));
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    terminator_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Terminate>
terminator<Terminate> make_terminator(Terminate&& t) {
    return {forward<Terminate>(t)};
}

namespace detail {

template<class T>
struct terminator_check : public false_type {};

template<class Adapt>
struct terminator_check<terminator<Adapt>> : public true_type {};

template<class T>
using for_terminator = enable_if_t<terminator_check<decay_t<T>>::value>;

template<class T>
using not_terminator = enable_if_t<!terminator_check<decay_t<T>>::value>;

}


inline context<> start(subscription lifetime = subscription{}) {
    return make_context(lifetime);
}

template<class Payload, class... AN>
auto start(AN&&... an) {
    return make_context<Payload>(subscription{}, forward<AN>(an)...);
}

template<class Payload, class... ArgN>
auto start(subscription lifetime, ArgN&&... an) {
    return make_context<Payload>(lifetime, forward<ArgN>(an)...);
}

template<class Payload, class Clock, class... AN>
auto start(AN&&... an) {
    return make_context<Payload, Clock>(subscription{}, forward<AN>(an)...);
}

template<class Payload, class Clock, class... AN>
auto start(subscription lifetime, AN&&... an) {
    return make_context<Payload, Clock>(lifetime, forward<AN>(an)...);
}

template<class Payload, class MakeStrand, class Clock>
auto start(const context<Payload, MakeStrand, Clock>& o) {
    return o;
}

template<class Payload, class MakeStrand, class Clock>
auto start(subscription lifetime, const context<Payload, MakeStrand, Clock>& o) {
    return copy_context(lifetime, o);
}

template<class... TN>
struct interface_extractor{
    template<class O>
    auto extract(O&& o){
        return o.template as_interface<TN...>();
    }
};
template<class... TN>
interface_extractor<TN...> as_interface() {
    return {};
}

const auto intervals = [](auto initial, auto period){
    info("new intervals");
    return make_observable([=](auto scrb){
        info("inttervals bound to subscriber");
        return make_starter([=](auto ctx) {
            auto r = scrb.create(ctx);
            info("intervals started");
            defer_periodic(ctx, initial, period, r);
            return ctx.lifetime;
        });
    });
};

const auto ints = [](auto first, auto last){
    info("new ints");
    return make_observable([=](auto scrb){
        info("ints bound to subscriber");
        return make_starter([=](auto ctx) {
            auto r = scrb.create(ctx);
            info("ints started");
            for(auto i = first;!ctx.lifetime.is_stopped(); ++i){
                r.next(i);
                if (i == last) break;
            }
            r.complete();
            return ctx.lifetime;
        });
    });
};

const auto copy_if = [](auto pred){
    info("new copy_if");
    return make_lifter([=](auto scbr){
        info("copy_if bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("copy_if bound to context");
            auto r = scbr.create(ctx);
            return make_observer(r, ctx.lifetime, [=](auto& r, auto v){
                if (pred(v)) r.next(v);
            });
        });
    });
};

const auto transform = [](auto f){
    info("new transform");
    return make_lifter([=](auto scbr){
        info("transform bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("transform bound to context");
            auto r = scbr.create(ctx);
            return make_observer(r, ctx.lifetime, [=](auto& r, auto v){
                r.next(f(v));
            });
        });
    });
};

const auto last_or_default = [](auto def){
        info("new last_or_default");
    return make_lifter([=](auto scbr){
        info("last_or_default bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("last_or_default bound to context");
            auto r = scbr.create(ctx);
            auto last = ctx.lifetime.template make_state<std::decay_t<decltype(def)>>(def);
            return make_observer(r, ctx.lifetime, 
                [last](auto& r, auto v){
                    last.get() = v;
                },
                detail::ignore{},
                [last](auto& r){
                    r.next(last.get());
                    r.complete();
                });
        });
    });
};

const auto take = [](int n){
    info("new take");
    return make_adaptor([=](auto source){
        info("take bound to source");
        return make_observable([=](auto scrb){
            info("take bound to subscriber");
            return source.bind(
                make_subscriber([=](auto ctx){
                    info("take bound to context");
                    auto r = scrb.create(ctx);
                    auto remaining = ctx.lifetime.template make_state<int>(n);
                    return make_observer(r, ctx.lifetime, 
                    [remaining](auto& r, auto v){
                        if (remaining.get()-- == 0) {
                            r.complete();
                            return;
                        }
                        r.next(v);
                    });
                }));
        });
    });
};

const auto merge = [](){
    info("new merge");
    return make_adaptor([=](auto source){
        info("merge bound to source");
        return make_observable([=](auto scrb){
            info("merge bound to subscriber");
            return source.bind(
                make_subscriber([=](auto ctx){
                    info("merge bound to context");
                    
                    auto pending = make_state<set<subscription>>(ctx.lifetime);
                    pending.get().insert(ctx.lifetime);

                    subscription destlifetime;
                    destlifetime.insert([pending](){
                        while (!pending.get().empty()) {
                            (*pending.get().begin()).stop();
                        }
                        info("merge-output stopped");
                    });
                    auto destctx = copy_context(destlifetime, ctx);
                    auto r = scrb.create(destctx);

                    ctx.lifetime.insert([pending, r, l = ctx.lifetime](){
                        pending.get().erase(l);
                        if (pending.get().empty()){
                            r.complete();
                        }
                        info("merge-input stopped");
                    });

                    return make_observer(r, destlifetime, 
                        [pending, destctx](auto r, auto v){
                            v.bind(
                                make_subscriber([=](auto ctx){
                                    info("merge-nested bound to context");
                                    pending.get().insert(ctx.lifetime);
                                    ctx.lifetime.insert([pending, r, l = ctx.lifetime](){
                                        pending.get().erase(l);
                                        if (pending.get().empty()){
                                            r.complete();
                                        }
                                        info("merge-nested stopped");
                                    });
                                    return make_observer(r, ctx.lifetime, 
                                        [pending](auto& r, auto v){
                                            r.next(v);
                                        },
                                        [](auto& r, auto e){
                                            r.error(e);
                                        },
                                        [](auto& r){
                                            // not complete until all pending streams have stopped
                                        });
                                })) | 
                            start(subscription{}, destctx);
                        },
                        [](auto& r, auto e){
                            r.error(e);
                        },
                        [](auto& r){
                            // not complete until all pending streams have stopped
                        });
                }));
        });
    });
};

template<class F>
auto transform_merge(F&& f) {
    return transform(forward<F>(f)) | merge();
};

const auto printto = [](auto& output){
    info("new printto");
    return make_subscriber([&](auto ctx) {
        info("printto bound to context");
        auto values = ctx.lifetime.template make_state<int>(0);
        return make_observer(
            ctx.lifetime,
            [&, values](auto v) {
                ++values.get();
                output << v << endl;
            },
            [&](exception_ptr ep){
                output << what(ep) << endl;
            },
            [&, values](){
                output << values.get() << " values received - done!" << endl;
            });
    });
};


/// \brief chain operator overload for
/// Subscriber = Lifter | Subscriber
/// \param lifter
/// \param subscriber
/// \returns subscriber
template<class Lifter, class Subscriber,
    class CheckL = detail::for_lifter<Lifter>,
    class CheckScbr = detail::for_subscriber<Subscriber>>
auto operator|(Lifter&& l, Subscriber&& scbr) {
    return l.lift(forward<Subscriber>(scbr));
}

/// \brief chain operator overload for
/// Lifter = Lifter | Lifter
/// \param lifter
/// \param lifter
/// \returns Lifter
template<class LifterL, class LifterR,
    class CheckLl = detail::for_lifter<LifterL>,
    class CheckLr = detail::for_lifter<LifterR>, 
    class _5 = void>
auto operator|(LifterL lhs, LifterR rhs) {
    return make_lifter([lhs = move(lhs), rhs = move(rhs)](auto&& scbr){
        lhs.lift(rhs.lift(forward<decltype(scbr)>(scbr)));
    });
}

/// \brief chain operator overload for
/// Observable = Observable | Lifter
/// \param observable
/// \param lifter
/// \returns observable
template<class Observable, class Lifter,
    class CheckS = detail::for_observable<Observable>,
    class CheckL = detail::for_lifter<Lifter>, 
    class _5 = void, 
    class _6 = void>
auto operator|(Observable&& s, Lifter&& l) {
    return make_observable([=](auto&& scrb){
        return s.bind(l.lift(forward<decltype(scrb)>(scrb)));
    });
}


/// \brief chain operator overload for
/// Starter = Observable | Subscriber
/// \param observable
/// \param subscriber
/// \returns starter
template<class Observable, class Subscriber,
    class CheckS = detail::for_observable<Observable>,
    class CheckScbr = detail::for_subscriber<Subscriber>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void>
auto operator|(Observable&& s, Subscriber&& scbr) {
    return s.bind(forward<Subscriber>(scbr));
}

/// \brief chain operator overload for
/// subscription = Starter | Context
/// \param starter
/// \param context
/// \returns subscription
template<class Starter, class Context,
    class CheckS = detail::for_starter<Starter>,
    class CheckCtx = detail::for_context<Context>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void>
subscription operator|(Starter&& s, Context&& ctx) {
    return s.start(forward<Context>(ctx));
}

/// \brief chain operator overload for
/// Adaptor = Adaptor | Adaptor
/// \param adaptor
/// \param adaptor
/// \returns adaptor
template<class AdaptorL, class AdaptorR,
    class CheckAL = detail::for_adaptor<AdaptorL>,
    class CheckAR = detail::for_adaptor<AdaptorR>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void>
auto operator|(AdaptorL&& lhs, AdaptorR&& rhs) {
    return make_adaptor([=](auto source){
        return rhs.adapt(lhs.adapt(source));
    });
}

/// \brief chain operator overload for
/// Adaptor = Adaptor | Lifter
/// \param adaptor
/// \param lifter
/// \returns adaptor
template<class Adapter, class Lifter,
    class CheckA = detail::for_adaptor<Adapter>,
    class CheckL = detail::for_lifter<Lifter>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void, 
    class _10 = void>
auto operator|(Adapter&& a, Lifter&& l) {
    return make_adaptor([=](auto source){
        auto s = a.adapt(source);
        return make_observable([=](auto&& scrb){
            return s.bind(l.lift(forward<decltype(scrb)>(scrb)));
        });
    });
}

/// \brief chain operator overload for
/// Adaptor = Lifter | Adaptor
/// \param lifter
/// \param adaptor
/// \returns adaptor
template<class Lifter, class Adapter,
    class CheckL = detail::for_lifter<Lifter>, 
    class CheckA = detail::for_adaptor<Adapter>,
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void, 
    class _10 = void, 
    class _11 = void>
auto operator|(Lifter&& l, Adapter&& a) {
    return make_adaptor([=](auto source){
        return a.adapt(make_observable([=](auto&& scrb){
            return source.bind(l.lift(forward<decltype(scrb)>(scrb)));
        }));
    });
}

/// \brief chain operator overload for
/// Observable = Observable | Adaptor
/// \param observable
/// \param adaptor
/// \returns observable
template<class Observable, class Adaptor,
    class CheckS = detail::for_observable<Observable>,
    class CheckA = detail::for_adaptor<Adaptor>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void, 
    class _10 = void, 
    class _11 = void, 
    class _12 = void>
auto operator|(Observable&& s, Adaptor&& a) {
    return a.adapt(forward<Observable>(s));
}

/// \brief chain operator overload for
/// Terminator = Adaptor | Subscriber
/// \param adaptor
/// \param subscriber
/// \returns terminator
template<class Adapter, class Subscriber,
    class CheckA = detail::for_adaptor<Adapter>,
    class CheckS = detail::for_subscriber<Subscriber>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void, 
    class _10 = void, 
    class _11 = void, 
    class _12 = void, 
    class _13 = void>
auto operator|(Adapter&& a, Subscriber&& scrb) {
    return make_terminator([=](auto source){
        return a.adapt(source).bind(scrb);
    });
}

/// \brief chain operator overload for
/// starter = Observable | Terminator
/// \param observable
/// \param terminator
/// \returns starter
template<class Observable, class Terminator,
    class CheckS = detail::for_observable<Observable>,
    class CheckA = detail::for_terminator<Terminator>, 
    class _5 = void, 
    class _6 = void, 
    class _7 = void, 
    class _8 = void, 
    class _9 = void, 
    class _10 = void, 
    class _11 = void, 
    class _12 = void, 
    class _13 = void, 
    class _14 = void>
auto operator|(Observable&& s, Terminator&& t) {
    return t.terminate(forward<Observable>(s));
}

/// \brief chain operator overload for
/// AnyInterface = Any | InterfaceExtractor
/// \param any
/// \param interface_extractor
/// \returns any_interface
template<class O, class... TN>
auto operator|(O&& o, interface_extractor<TN...>&& ie) {
    return ie.extract(forward<O>(o));
}


}

extern"C" {
    void designcontext(int, int);
}

void designcontext(int first, int last){
    using namespace std::chrono;

    using namespace rx;
    using rx::copy_if;
    using rx::transform;
    using rx::merge;
 
{
 cout << "compile-time polymorphism" << endl;
    auto lastofeven = copy_if(even) | 
        take(100000000) |
        last_or_default(42);

 auto t0 = high_resolution_clock::now();
    auto lifetime = ints(0, 2) | 
        transform_merge([=](int){
            return ints(first, last * 100) |
                lastofeven;
        }) |
        printto(cout) |
        start<destruction>();

    lifetime.insert([](){info("caller stopped");});

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = ((last * 100) - first) * 3;
 cout << d / sc << " ms per value\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " values per second\n"; 
}

{
 cout << "intervals" << endl;
    auto threeeven = copy_if(even) | 
        take(3);

    auto lifetime = intervals(steady_clock::now(), 1s) | 
        threeeven |
        printto(cout) |
        start<destruction>();

    lifetime.insert([](){cout << "caller stopped" << endl;});
}

#if 0
{
 cout << "interface polymorphism" << endl;
    auto lastofeven = copy_if(even) | 
        as_interface<int>() |
        take(100000000) |
        as_interface<int>() |
        last_or_default(42) |
        as_interface<int>();
        
 auto t0 = high_resolution_clock::now();
    auto lifetime = ints(0, 2) | 
        as_interface<int>() |
        transform_merge([=](int){
            return ints(first, last * 100) |
                as_interface<int>() |
                lastofeven |
                as_interface<int>();
        }) |
        as_interface<int>() |
        printto(cout) |
        as_interface<int>() |
        start<destruction>();

    lifetime.insert([](){info("caller stopped");});
 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = ((last * 100) - first) * 3;
 cout << d / sc << " ms per value\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " values per second\n"; 
}
#endif

{
 cout << "for" << endl;
 auto t0 = high_resolution_clock::now();
    for(auto i = first; i < last; ++i) {
        auto lifetime = ints(0, 0) |
            transform([](int i) {
                return to_string(i);
            }) |
            transform([](const string& s) {
                int i = '0' - s[0];
                return i;
            }) |
        make_subscriber() |
        start<destruction>();

        lifetime.insert([](){info("caller stopped");});
    }
 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}

{
 cout << "transform | merge" << endl;
 auto t0 = high_resolution_clock::now();

    auto lifetime = ints(first, last) | 
        transform([=](int){
            return ints(0, 0) |
                transform([](int i) {
                    return to_string(i);
                }) |
                transform([](const string& s) {
                    int i = '0' - s[0];
                    return i;
                });
        }) |
        merge() |
        make_subscriber() |
        start<destruction>();

    lifetime.insert([](){info("caller stopped");});

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}

{
 cout << "transform_merge" << endl;
 auto t0 = high_resolution_clock::now();

    auto lifetime = ints(first, last) | 
        transform_merge([=](int){
            return ints(0, 0) |
                transform([](int i) {
                    return to_string(i);
                }) |
                transform([](const string& s) {
                    int i = '0' - s[0];
                    return i;
                });
        }) |
        make_subscriber() |
        start<destruction>();

    lifetime.insert([](){info("caller stopped");});

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}

}
