#include "womp/App.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        womp::App app;
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "womp: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
