/*
 * Project Project: main.cpp
 * Created: 2024/6/27
 * This program uses MulanPSL2. See LICENSE for more.
 */

#include <cstdio>
#include <fstream>

struct SomeS {
    float x{};
    float y{1};
};

int main () {
    SomeS s {};
    printf("%f\n", s.y);

    s = {};
    printf("%f\n", s.y);
}