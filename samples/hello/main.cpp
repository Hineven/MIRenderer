/*
 * Project Project: main.cpp
 * Created: 2024/6/27
 * This program uses MulanPSL2. See LICENSE for more.
 */

#include <cstdio>
#include <fstream>

class Base {
public:
    virtual void Print() {
        printf("Base\n");
    }
    virtual void PurePrint () = 0;
    void Reset () {
        printf("Reset\n");
        Print();
    }
    void PureReset () {
        printf("PureReset\n");
        PurePrint();
    }
};

class Derived : public Base {
public:
    void Print() override {
        printf("Derived\n");
    }
    void PurePrint () override {
        printf("PureDerived\n");
    }
};


int main () {
    Derived var;
    var.Print();
    var.PurePrint();
    var.Reset();
    var.PureReset();
}