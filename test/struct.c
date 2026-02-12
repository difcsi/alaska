
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

// typedef struct alaska_typeinfo alaska_typeinfo_t;
// typedef struct alaska_typemember alaska_typemember_t;

// struct alaska_typemember {
//   const char *name;
//   size_t offset;
//   alaska_typeinfo_t *type;
// };

// struct alaska_typeinfo {
//   const char *name;
//   size_t byte_size;
//   unsigned long flags;
//   size_t member_count;
//   alaska_typemember_t *members;  // can be null;
// };

// #define ALASKA_TYPE(name) alaska_type_##name

// #define DEFINE_PRIMITIVE(type_name, c_type)                                \
//   alaska_typeinfo_t ALASKA_TYPE(type_name) = {.name = #type_name,          \
//                                               .byte_size = sizeof(c_type), \
//                                               .flags = 0,                  \
//                                               .member_count = 0,           \
//                                               .members = NULL};

// #define BEGIN_STRUCT(name, c_type) alaska_typemember_t alaska_members_##name[] = {
// #define FIELD(fname, ftype, container) \
//   {.name = #fname, .offset = offsetof(container, fname), .type = &ALASKA_TYPE(ftype)},

// #define END_STRUCT(type_name, c_type) \
//   } \
//   ; \
//   alaska_typeinfo_t ALASKA_TYPE(type_name) = { \
//       .name = #type_name, \
//       .byte_size = sizeof(c_type), \
//       .flags = 0, \
//       .member_count = sizeof(alaska_members_##type_name) / sizeof(alaska_members_##type_name[0]),
//       \ .members = alaska_members_##type_name};

// // Define primitive types
// DEFINE_PRIMITIVE(int, int)
// DEFINE_PRIMITIVE(char, char)

struct thing {
  char c;
  int x;
};

// // Define struct type info
// BEGIN_STRUCT(thing, struct thing)
// FIELD(c, char, struct thing)
// FIELD(x, int, struct thing)
// END_STRUCT(thing, struct thing)


// void print_type(alaska_typeinfo_t *type) {
//   if (!type) {
//     printf("nil");
//     return;
//   }
//   printf("(%s :size %zu", type->name, type->byte_size);
//   if (type->member_count > 0) {
//     printf(" :members");
//     for (size_t i = 0; i < type->member_count; i++) {
//       alaska_typemember_t *m = &type->members[i];
//       printf(" (%s :offset %zu :type ", m->name, m->offset);
//       print_type(m->type);
//       printf(")");
//     }
//   }
//   printf(")");
// }


struct thing *foo() {
  struct thing *t = malloc(sizeof(*t));
  t->c = 'a';
  t->x = 1;
  return t;
}

int main() {
  // print_type(&ALASKA_TYPE(thing));
  // printf("\n");

  return 0;
}