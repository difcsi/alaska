#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct node_s {
  volatile int value;
  struct node_s *next;
} node_t;


node_t *search(node_t *n, int value) {
  while (n) {
    if (n->value == value) return n;
    n = n->next;
  }

  return 0;
}

node_t *make_list(int len) {
  if (len == 0) return 0;

  node_t *n = (node_t*)calloc(1, sizeof(*n));
  n->next = make_list(len - 1);
  n->value = 0;

  return n;
}


void walk(node_t *n) {
  if (n == 0) return;
  n->value++;
  walk(n->next);
  n->value--;
}

int main() {

  node_t *list = make_list(512);

  while (1) {
    walk(list);
  }
}


