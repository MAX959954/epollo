#include <iostream>

#include "epollo/version.hpp"

int main() {
    std::cout << "epollo " << epollo::version() << '\n';
    return 0;
}
