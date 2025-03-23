#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <alaska.h>
#include <stdbool.h>

typedef struct node {
  struct node *left, *right;
  int value;
} node_t;


node_t *make_tree(int depth) {
  if (depth == 0) return NULL;

  node_t *n = calloc(1, sizeof(*n));

  n->left = make_tree(depth - 1);
  n->right = make_tree(depth - 1);
  n->value = rand();

  return n;
}


void free_tree(node_t *n) {
  if (n == NULL) return;

  free_tree(n->left);
  free_tree(n->right);
  free(n);
}


int tree_count_nodes(node_t *n) {
  if (n == NULL) return 0;
  return 1 + tree_count_nodes(n->left) + tree_count_nodes(n->right);
}

bool localize_structure(uint64_t ptr);

int main() {
  long start, end;
  printf("localized,walk_time\n");
  node_t *n = make_tree(18);
  for (int trial = 0; trial < 4000; trial++) {
    bool localized = false;


    localize_structure((uint64_t)n);
    int c = 0;
    start = alaska_timestamp();
    for (int i = 0; i < 200; i++) {
      c += tree_count_nodes(n);
    }
    end = alaska_timestamp();
    uint64_t walk_time = end - start;
    printf("%d,%f\n", localized, walk_time / 200.0);
    // printf("Node count: %d\n", c);
  }
  free_tree(n);

  return EXIT_SUCCESS;
}
