
#include "sqlite3.h"
#include <stdio.h>

int main(void) {
  void *p = sqlite3_malloc(32);
  sqlite3_free(p);
  return 0;
}
