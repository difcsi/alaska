#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <alaska.h>
#include <stdbool.h>

typedef struct node {
  struct node *next;
  int value;
  // char *payload;
} node_t;


unsigned int seed = 12345;


node_t *make_list(int depth) {
  node_t *list = null;

  while (depth > 0) {
    node_t *n = calloc(1, sizeof(node_t));

    n->next = list;
    list = n;
    depth--;
  }

  return list;
}

__attribute__((noinline)) int list_count_nodes(volatile node_t *n) {
  int count = 0;

  while (n != null) {
    count++;
    n = n->next;
  }
  return count;
}


void free_list(node_t *n) {
  if (n == null) return;

  while (n != null) {
    node_t *cur = n;
    n = n->next;
    free(cur);
  }
}

bool localize_structure(uint64_t ptr);



void run_tests(node_t *n) {
  for (int trial = 0; trial < 15; trial++) {
    volatile unsigned long c = 0;
    uint64_t start = alaska_timestamp();
    for (int i = 0; i < 200; i++) {
      c += list_count_nodes(n);
    }
    uint64_t end = alaska_timestamp();

    long walk_time = end - start;

    (void)c;
    printf("%d, %fs\n", trial, walk_time / 1e9f);
    // return 0;
    // printf("node count: %lu\n", c);
  }
}


int main() {
  long start, end;
  node_t *n = make_list(1 << 21);
  printf("localized,walk_time\n");
  bool localized = false;
  // run_tests(n);
  // localized = localize_structure((uint64_t)n);
  run_tests(n);
  free_list(n);

  return exit_success;
}
