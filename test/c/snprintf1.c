#include "sqlite3.h"
#include <stdio.h>

int main(void) {
    char szBuffer[32];
    double val = 2023.0;
    sqlite3_snprintf(17, szBuffer, "%.3f", val);
    printf("size 17: '%s'\n", szBuffer);
    sqlite3_snprintf(16, szBuffer, "%.3f", val);
    printf("size 16: '%s'\n", szBuffer);
    return 0;
}
