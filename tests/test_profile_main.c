#include <stdio.h>

#include "test_support.h"

int main(void)
{
    const int result = test_profile();

    if (result == 0) {
        printf("UCN profile tests passed.\n");
    }
    return result;
}
