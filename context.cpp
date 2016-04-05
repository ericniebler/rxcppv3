
// source ~/source/emsdk_portable/emsdk_env.sh

// em++ -std=c++14 --memory-init-file 0 -s ASSERTIONS=2 -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_designcontext']" -Iexecutors/include -DRX_INFO=0 -DRX_SKIP_TESTS=0 -DRX_SLOW=0 -DRX_DEFER_IMMEDIATE=0 -O2 context.cpp -o context.js

// c++ -std=c++14 -DRX_INFO=0 -DRX_SKIP_TESTS=0 -DRX_SLOW=0 -DRX_DEFER_IMMEDIATE=0 -O2 context.cpp -o context

#if EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include <set>
#include <map>
#include <list>
#include <string>
#include <iostream>
#include <iomanip>
#include <exception>

#include <regex>
#include <random>
#include <chrono>
#include <thread>
#include <sstream>
#include <future>
#include <queue>
using namespace std;
using namespace std::chrono;
using namespace std::literals;

inline string what(exception_ptr ep) {
    try {rethrow_exception(ep);}
    catch (const exception& ex) {
        return ex.what();
    }
    return string();
}

namespace detail {

mutex infolock;
const auto start = steady_clock::now();

void info(){
    cout << endl;
}

template<class A0, class... AN>
void info(A0 a0, AN... an){
    cout << a0;
    info(an...);
}

}

const auto info = [](auto... an){
#if RX_INFO
    unique_lock<mutex> guard(detail::infolock);
    cout << this_thread::get_id() << " - " << duration_cast<milliseconds>(steady_clock::now() - detail::start).count() << "ms - ";
    detail::info(an...);
#else
    make_tuple(an...);
#endif
};

const auto output = [](auto... an){
#if RX_INFO
    unique_lock<mutex> guard(detail::infolock);
#endif
    cout << this_thread::get_id() << " - " << duration_cast<milliseconds>(steady_clock::now() - detail::start).count() << "ms - ";
    detail::info(an...);
};

auto even = [](auto v){return (v % 2) == 0;};

auto always_throw = [](auto... ){
    throw runtime_error("always throw!");
    return true;
};
void use_to_silence_compiler(){
    always_throw();
    even(0);
}

struct destruction
{
    struct Track 
    {
        ~Track(){
            output("destructed");
        }
    };
    shared_ptr<Track> track;
    destruction() : track(make_shared<Track>()) {}
};

#include "common.h"
#include "rx.h"

int main() {
    //emscripten_set_main_loop(tick, -1, false);
    designcontext(0, 10000);
    output("main sleep");
    this_thread::sleep_for(2s);
    output("main exit");
    return 0;
}

