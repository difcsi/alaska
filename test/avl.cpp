#include <unistd.h>
#include <memory>
#include <stdlib.h>
#include <stdio.h>

template <typename K, typename V>
struct Node {
  Node *left, *right;
  K key;
  V value;
  int height;

  Node(K k, V v)
      : key(k)
      , value(v)
      , height(1)
      , left(nullptr)
      , right(nullptr) {}
};

static unsigned long bytes_allocated = 0;

template <typename K, typename V>
struct TreeMap {
  using NodePtr = Node<K, V> *;
  NodePtr root = nullptr;

  int height(NodePtr n) { return n ? n->height : 0; }

  int balance_factor(NodePtr n) { return n ? height(n->left) - height(n->right) : 0; }

  NodePtr rotate_right(NodePtr y) {
    NodePtr x = y->left;
    NodePtr T2 = x->right;

    x->right = y;
    y->left = T2;

    y->height = std::max(height(y->left), height(y->right)) + 1;
    x->height = std::max(height(x->left), height(x->right)) + 1;

    return x;
  }

  NodePtr rotate_left(NodePtr x) {
    NodePtr y = x->right;
    NodePtr T2 = y->left;

    y->left = x;
    x->right = T2;

    x->height = std::max(height(x->left), height(x->right)) + 1;
    y->height = std::max(height(y->left), height(y->right)) + 1;

    return y;
  }

  NodePtr insert(NodePtr node, K key, V value) {
    if (!node) {
      size_t size = sizeof(Node<K, V>);
      auto *x = (NodePtr)calloc(1, size);
      bytes_allocated += size;
      x->left = nullptr;
      x->right = nullptr;
      x->key = key;
      x->value = value;
      x->height = 1;
      return x;
    }

    if (key < node->key) {
      node->left = insert(node->left, key, value);
    } else if (key > node->key) {
      node->right = insert(node->right, key, value);
    } else {
      node->value = value;
      return node;
    }

    node->height = 1 + std::max(height(node->left), height(node->right));

    int balance = balance_factor(node);

    if (balance > 1 && key < node->left->key) {
      return rotate_right(node);
    }

    if (balance < -1 && key > node->right->key) {
      return rotate_left(node);
    }

    if (balance > 1 && key > node->left->key) {
      node->left = rotate_left(node->left);
      return rotate_right(node);
    }

    if (balance < -1 && key < node->right->key) {
      node->right = rotate_right(node->right);
      return rotate_left(node);
    }

    return node;
  }

  void insert(K key, V value) { root = insert(root, key, value); }

  NodePtr lookup(K key) {
    auto cur = root;
    while (cur) {
      if (key < cur->key) {
        cur = cur->left;
      } else if (key > cur->key) {
        cur = cur->right;
      } else {
        return cur;
      }
    }
    return nullptr;
  }

  void dump_node(NodePtr n, int depth = 0) {
    if (!n) return;

    if (depth == 0) {
      printf("%d [label=\"ROOT %d\"];\n", n->key, n->key);
    }

    if (n->left) {
      printf("%d -> %d;\n", n->key, n->left->key);
      dump_node(n->left, depth + 1);
    }
    if (n->right) {
      printf("%d -> %d;\n", n->key, n->right->key);
      dump_node(n->right, depth + 1);
    }
  }
};


int main() {
  long node_count = 1'000'000;
  TreeMap<int, int> t;
  printf("Inserting...\n");
  for (int i = 0; i < node_count; i++) {
    t.insert(i, rand());
  }
  printf("done. allocated %fmb.\n", bytes_allocated / 1024.0 / 1024.0);

  while (1) {
    printf("random lookups...\n");
    for (int j = 0; j < 1'000'000; j++) {
      auto ind = rand() % node_count;
      auto n = t.lookup(ind);
    }
  }
  return 0;
}
