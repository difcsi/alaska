#include <stdlib.h>
#include <stdio.h>

typedef struct node {
  long value;
  long num_edges;
  struct node *edges[];
} node_t;


node_t *make_node(long num_edges) {
  node_t *n = (node_t *)malloc(sizeof(node_t) + sizeof(node_t *) * num_edges);
  n->value = rand();
  n->num_edges = num_edges;
  for (long i = 0; i < num_edges; i++) {
    n->edges[i] = NULL;
  }
  return n;
}

node_t *build_graph(node_t *nodes[], long num_nodes, long max_edges) {
  for (long i = 0; i < num_nodes; i++) {
    long max_random = max_edges > 1 ? max_edges - 1 : 1;
    long num_edges = rand() % max_random + 1;
    nodes[i] = make_node(num_edges);
  }

  if (num_nodes == 0) {
    return NULL;
  }

  for (long i = 0; i < num_nodes - 1; i++) {
    node_t *current = nodes[i];
    if (current->num_edges > 0) {
      current->edges[0] = nodes[i + 1];
    }
  }

  if (nodes[num_nodes - 1]->num_edges > 0) {
    nodes[num_nodes - 1]->edges[0] = nodes[0];
  }

  for (long i = 0; i < num_nodes; i++) {
    node_t *n = nodes[i];
    for (long j = 1; j < n->num_edges; j++) {
      long target = rand() % num_nodes;
      n->edges[j] = nodes[target];
    }
  }

  return nodes[0];
}

void clear_visited(node_t *nodes[], long num_nodes) {
  for (long i = 0; i < num_nodes; i++) {
    // nodes[i]->visited = 0;
  }
}

int main() {
  long num_nodes = 100000;
  long max_edges = 10;

  node_t *nodes[num_nodes];

  node_t *root = build_graph(nodes, num_nodes, max_edges);

  clear_visited(nodes, num_nodes);

  if (!root) {
    return 0;
  }

  printf("Root node value=%ld\n", root->value);

  // Print the graph
  for (long i = 0; i < num_nodes; i++) {
    node_t *n = nodes[i];
    printf("Node %ld: value=%ld, edges=[", i, n->value);
    for (long j = 0; j < n->num_edges; j++) {
      if (n->edges[j]) {
        printf("%ld", n->edges[j]->value);
      } else {
        printf("NULL");
      }
      if (j < n->num_edges - 1) {
        printf(", ");
      }
    }
    printf("]\n");
  }

  // Free the graph
  for (long i = 0; i < num_nodes; i++) {
    free(nodes[i]);
  }

  return 0;
}
