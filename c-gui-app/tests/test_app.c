#include <stdio.h>
#include <assert.h>
#include "app.h"

void test_addition() {
    int result = add(2, 3);
    assert(result == 5);
}

void test_subtraction() {
    int result = subtract(5, 3);
    assert(result == 2);
}

int main() {
    test_addition();
    test_subtraction();
    
    printf("All tests passed!\n");
    return 0;
}