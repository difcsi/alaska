#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <alaska.h>

#define MAX_SIZE 512
#define ARRAY_LENGTH (4096 * 1024 * 2)
void *objects[ARRAY_LENGTH];

#define OBJ_SIZE 24

extern void alaska_dump(void);


int main(int argc, char **argv) {

  int count = 5;

  if (argc == 2) {
    count = atoi(argv[1]);
  }


  for (int run = 0; run < count; run++) {
    // printf("Allocate...\n");
    unsigned int seed = 0;


    uint64_t start = alaska_timestamp();

#pragma omp parallel for shared(objects) private(seed)
    for (int i = 0; i < ARRAY_LENGTH; i++) {
      objects[i] = malloc(OBJ_SIZE);
      // objects[i] = malloc(256);
    }

    // alaska_dump();

    // printf("Free...\n");
#pragma omp parallel for shared(objects)
    for (int i = 0; i < ARRAY_LENGTH; i++) {
      free(objects[i]);
      objects[i] = NULL;
    }

    uint64_t end = alaska_timestamp();
    printf("%zu ms\n", (end - start) / 1000 / 1000);


    memset(objects, 0, sizeof(objects));
  }

  return 0;
}


// 3e1f43dc009fab8
// 3e1f43dc009fab8
// 3e1e90c4cc82560
