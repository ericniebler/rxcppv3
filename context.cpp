
// source ~/source/emsdk_portable/emsdk_env.sh

// em++ -std=c++14 --memory-init-file 0 -s ASSERTIONS=2 -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_designcontext']" -Iexecutors/include -O2 context.cpp -o context.js

// c++ -std=c++14 -DRX_INFO=1 -O2 context.cpp -o context

#if EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include <set>
#include <deque>
#include <string>
#include <iostream>
#include <iomanip>
#include <exception>

#include <regex>
#include <random>
#include <chrono>
#include <thread>
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
    detail::info(an...);
#endif
};

auto even = [](auto v){return (v % 2) == 0;};

auto always_throw = [](auto... ){
    throw runtime_error("always throw!");
    return true;
};

struct destruction
{
    bool moved = false;
    destruction() = default;
    destruction(const destruction& o) = default;
    destruction(destruction&& o){
        o.moved = true;
    }
    destruction& operator=(const destruction& o) = default;
    destruction& operator=(destruction&& o) {
        o.moved = true;
        return *this;
    }
    
    ~destruction(){
        if (!moved) {
            cout << "destructed" << endl;
        }
    }
};

#include "common.h"
#include "rx.h"

int main() {
    //emscripten_set_main_loop(tick, -1, false);
    designcontext(0, 1000000);
    return 0;
}

